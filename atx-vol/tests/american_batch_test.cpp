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
#include "atx/vol/counters.hpp"
#include "atx/vol/simd/cpu.hpp"

#include <atomic>
#include <barrier>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <thread>
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

// The explicit-ISA overload is the concurrency-safe boundary between a public
// PricingKernel and the SIMD implementation. Unlike the legacy overload, it must
// select its route from the call argument without touching the startup/CI knob.
TEST(AmericanBoundaryBatch, PerCallIsaDoesNotMutateProcessOverride) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on this host";
  }
  IsaGuard g;
  simd::set_simd_isa_override(simd::SimdIsa::Auto);
  const Book b = make_book();
  constexpr std::size_t n = 30; // the leading lanes are genuine American puts
  ASSERT_GE(b.size(), n);
  std::vector<double> scalar_price(n);
  std::vector<double> avx2_price(n);

  const simd::SimdRoute scalar_route = simd::american_put_boundary_batch(
      b.S.data(), b.K.data(), b.T.data(), b.sigma.data(), b.r.data(), b.q.data(),
      scalar_price.data(), n, simd::SimdIsa::ForceScalar);
  EXPECT_EQ(scalar_route, simd::SimdRoute::Scalar);
  EXPECT_EQ(simd::simd_isa_override(), simd::SimdIsa::Auto);

  const simd::SimdRoute avx2_route = simd::american_put_boundary_batch(
      b.S.data(), b.K.data(), b.T.data(), b.sigma.data(), b.r.data(), b.q.data(), avx2_price.data(),
      n, simd::SimdIsa::ForceAvx2);
  EXPECT_EQ(avx2_route, simd::SimdRoute::Avx2);
  EXPECT_EQ(simd::simd_isa_override(), simd::SimdIsa::Auto);
}

TEST(AmericanBoundaryBatch, PerCallAutoUsesShipGateWithoutMutatingGlobalOverride) {
  IsaGuard g;
  // Set the process override to the OPPOSITE of what Auto now picks. With the ship gate ON
  // (kShipAvx2Boundary=true, PM 2026-07-19) Auto routes AVX2 on an AVX2-capable host, so we
  // force the global override to Scalar and prove the per-call Auto route is call-local: it
  // neither reads nor mutates the global override, and follows the ship gate (AVX2).
  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
  const Book b = make_book();
  constexpr std::size_t n = 1;
  ASSERT_GE(b.size(), n);
  std::vector<double> price(n);

  const simd::SimdRoute route = simd::american_put_boundary_batch(
      b.S.data(), b.K.data(), b.T.data(), b.sigma.data(), b.r.data(), b.q.data(), price.data(), n,
      simd::SimdIsa::Auto);

  // Ship gate ON: the DEFAULT Auto route follows the gate — AVX2 on an AVX2-capable host,
  // scalar otherwise — without reading or mutating the ForceScalar process override set above.
  EXPECT_EQ(route, simd::have_avx2() ? simd::SimdRoute::Avx2 : simd::SimdRoute::Scalar);
  EXPECT_EQ(simd::simd_isa_override(), simd::SimdIsa::ForceScalar);
}

TEST(AmericanBoundaryBatch, ForceAvx2IsCapabilityGuarded) {
  const Book b = make_book();
  constexpr std::size_t n = 1;
  std::vector<double> price(n);

  const simd::SimdRoute route = simd::american_put_boundary_batch(
      b.S.data(), b.K.data(), b.T.data(), b.sigma.data(), b.r.data(), b.q.data(), price.data(), n,
      simd::SimdIsa::ForceAvx2);

  EXPECT_EQ(route, simd::have_avx2() ? simd::SimdRoute::Avx2 : simd::SimdRoute::Scalar);
}

TEST(AmericanBoundaryBatch, LegacyOverloadStillUsesProcessOverride) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on this host";
  }
  IsaGuard g;
  const Book b = make_book();
  constexpr std::size_t n = 30;
  ASSERT_GE(b.size(), n);
  std::vector<double> price(n);

  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
  EXPECT_EQ(simd::american_put_boundary_batch(b.S.data(), b.K.data(), b.T.data(), b.sigma.data(),
                                              b.r.data(), b.q.data(), price.data(), n),
            simd::SimdRoute::Scalar);

  simd::set_simd_isa_override(simd::SimdIsa::ForceAvx2);
  EXPECT_EQ(simd::american_put_boundary_batch(b.S.data(), b.K.data(), b.T.data(), b.sigma.data(),
                                              b.r.data(), b.q.data(), price.data(), n),
            simd::SimdRoute::Avx2);
}

TEST(AmericanBoundaryBatch, ConcurrentPerCallIsaSelectionsRetainOwnRoutes) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on this host";
  }
  IsaGuard g;
  simd::set_simd_isa_override(simd::SimdIsa::Auto);
  const Book b = make_book();
  constexpr std::size_t n = 30;
  constexpr std::size_t kIterations = 64;
  ASSERT_GE(b.size(), n);
  std::vector<double> scalar_price(n);
  std::vector<double> avx2_price(n);
  std::barrier<> rendezvous{2};
  std::atomic<bool> scalar_wrong_route{false};
  std::atomic<bool> avx2_wrong_route{false};

  {
    std::jthread scalar_thread([&] {
      for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        rendezvous.arrive_and_wait();
        const simd::SimdRoute route = simd::american_put_boundary_batch(
            b.S.data(), b.K.data(), b.T.data(), b.sigma.data(), b.r.data(), b.q.data(),
            scalar_price.data(), n, simd::SimdIsa::ForceScalar);
        if (route != simd::SimdRoute::Scalar) {
          scalar_wrong_route.store(true);
        }
        rendezvous.arrive_and_wait();
      }
    });
    std::jthread avx2_thread([&] {
      for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        rendezvous.arrive_and_wait();
        const simd::SimdRoute route = simd::american_put_boundary_batch(
            b.S.data(), b.K.data(), b.T.data(), b.sigma.data(), b.r.data(), b.q.data(),
            avx2_price.data(), n, simd::SimdIsa::ForceAvx2);
        if (route != simd::SimdRoute::Avx2) {
          avx2_wrong_route.store(true);
        }
        rendezvous.arrive_and_wait();
      }
    });
  }

  EXPECT_FALSE(scalar_wrong_route.load());
  EXPECT_FALSE(avx2_wrong_route.load());
  EXPECT_EQ(simd::simd_isa_override(), simd::SimdIsa::Auto);
}

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

TEST(AmericanPriceBatch, DefaultKernelRoutesShipGatedAvx2Lanes) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on this host";
  }
  IsaGuard g;
  // Ship gate ON (kShipAvx2Boundary=true, PM 2026-07-19): the DEFAULT (Auto) kernel follows
  // the gate — AVX2 on the regular whole-pack lanes, exact scalar on the irregular / patched /
  // n%4 tail lanes. Prove it is call-local too: it neither reads nor mutates the global
  // override, set here to the OPPOSITE (ForceScalar).
  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
  const Book b = make_book();
  const std::size_t n = b.size();
  PricingKernel kernel;
  ASSERT_EQ(kernel.isa, simd::SimdIsa::Auto);
  PricingWorkspace ws;
  PriceBatchOutput out;

  ASSERT_TRUE(american_price_batch(b.view(), out, kernel, ws).has_value());
  ASSERT_EQ(out.size(), n);

  std::size_t n_avx2 = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double want = ref_price(b, i);
    if (out.route[i] == simd::SimdRoute::Avx2) {
      ++n_avx2;  // regular whole-pack lane rode the AVX2 boundary kernel
      ASSERT_FALSE(std::isnan(want)) << "i=" << i;
      EXPECT_LE(std::abs(out.price[i] - want), 1e-3) << "i=" << i;  // T13 stress gate
      EXPECT_EQ(out.status[i], LaneStatus::Ok) << "i=" << i;
    } else {
      // Irregular / patched / n%4 tail lanes take the exact scalar andersen_lake.
      EXPECT_EQ(out.route[i], simd::SimdRoute::Scalar) << "i=" << i;
      if (std::isnan(want)) {
        EXPECT_TRUE(std::isnan(out.price[i])) << "i=" << i;
      } else {
        EXPECT_EQ(out.price[i], want) << "i=" << i;
      }
    }
  }
  EXPECT_GT(n_avx2, 0u);                          // genuine American lanes exercised the vector kernel
  EXPECT_LT(out.scalar_fallback_rate(), 1.0);
  EXPECT_EQ(simd::simd_isa_override(), simd::SimdIsa::ForceScalar);  // untouched
}

// ── The pack-dispatch counter observes THIS entry too (REVWSA finding 6). ────
// `AmericanAvxPackDispatches` used to be bumped only inside
// american_price_batch_resolved, so the complete AVX2 packs dispatched from
// american_price_batch — the laned flagship, and the entry the Python binding calls
// — were invisible to it. The counter's name promises "packs actually dispatched to
// AVX2"; it delivered "packs dispatched by one of the two entries". The routing half
// of this test runs in every build; the count half needs ATX_VOL_COUNTERS=ON.
TEST(AmericanPriceBatch, AvxPackDispatchesAreCountedOnThisEntryToo) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on this host";
  }
  IsaGuard g;
  // 14 genuine American put lanes (r > q > 0, T > 0, sigma > 0 -> is_kernel_lane) =>
  // floor(14/4) = 3 complete 4-lane packs, and a 2-lane tail the AVX2 driver prices
  // scalar itself (american_boundary_avx2.cpp:104) without dispatching.
  Book b;
  for (double m : {0.8, 0.9, 0.95, 1.0, 1.05, 1.1, 1.25}) {
    for (double t : {0.25, 0.75}) {
      b.push(100.0, 100.0 * m, t, 0.25, 0.05, 0.01, Side::Put);
    }
  }
  ASSERT_EQ(b.size(), 14u);

  PricingKernel kernel;
  kernel.isa = simd::SimdIsa::ForceAvx2;
  PricingWorkspace ws;
  PriceBatchOutput out;
  if constexpr (counters::counters_enabled()) {
    counters::reset();
  }
  ASSERT_TRUE(american_price_batch(b.view(), out, kernel, ws).has_value());

  // Anti-vacuity: all 14 must be kernel lanes, else m — and therefore m/4 — is not
  // the number this test claims to pin.
  std::size_t n_pack_lanes = 0;
  for (std::size_t i = 0; i < b.size(); ++i) {
    n_pack_lanes += (out.route[i] == simd::SimdRoute::Avx2) ? 1u : 0u;
  }
  ASSERT_EQ(n_pack_lanes, 14u) << "every lane must reach the pack for the count below";

  if constexpr (counters::counters_enabled()) {
    // Printed, not merely asserted: under the gate's counters-OFF build this whole
    // block vanishes, so a counters-ON run is the ONLY thing that ever observes the
    // bump (REVA7FIX §7). Emitting the value makes such a run self-documenting
    // instead of leaving "the assertion did not fail" as the only evidence.
    const std::uint64_t packs =
        counters::snapshot().get(counters::Counter::AmericanAvxPackDispatches);
    std::printf("[REVA7FIX] american_price_batch AVX2 pack dispatches = %llu "
                "(expected floor(14/4) = 3)\n",
                static_cast<unsigned long long>(packs));
    EXPECT_EQ(packs, 3u);
  }
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

TEST(ResolvedAmericanPriceBatch, ExactMethodsOptionsMixedSidesAndLaneErrors) {
  const std::vector<double> strikes{80.0, 95.0, 100.0, 110.0, -1.0};
  const std::vector<double> sigma{0.18, 0.24, 0.31, 0.42, 0.20};
  const std::vector<Side> sides{Side::Put, Side::Call, Side::Put, Side::Call, Side::Put};
  std::vector<double> prices(strikes.size());
  std::vector<Status> status(strikes.size());
  std::vector<simd::SimdRoute> route(strikes.size());

  struct Case {
    AmericanMethod method;
    std::optional<AlOpts> opts;
  };
  const std::vector<Case> cases{
      {AmericanMethod::AndersenLake, std::optional<AlOpts>{al_fast_opts()}},
      {AmericanMethod::AndersenLake,
       std::optional<AlOpts>{AlOpts{/*n_collocation=*/9, /*n_quadrature=*/32,
                                    /*max_newton_iter=*/6, /*tol=*/3.0e-9}}},
      {AmericanMethod::Baw, std::optional<AlOpts>{al_fast_opts()}},
  };

  for (const Case& c : cases) {
    const ResolvedAmericanPriceBatchRequest request{
        .S = 100.0,
        .T = 0.73,
        .r = 0.04,
        .q = 0.06,
        .K = strikes,
        .sigma = sigma,
        .side = sides,
        .method = c.method,
        .al_opts = c.opts,
        .isa = simd::SimdIsa::ForceScalar,
        .price = prices,
        .status = status,
        .pack_dispatch = route,
    };
    ASSERT_TRUE(american_price_batch_resolved(request).has_value());
    for (std::size_t i = 0; i < strikes.size(); ++i) {
      const Result<double> expected =
          american_price(100.0, strikes[i], 0.73, sigma[i], 0.04, 0.06, sides[i], c.method, c.opts);
      EXPECT_EQ(status[i].has_value(), expected.has_value()) << i;
      EXPECT_EQ(route[i], simd::SimdRoute::Scalar) << i;
      if (expected.has_value()) {
        EXPECT_EQ(prices[i], *expected) << i;
      } else {
        EXPECT_TRUE(std::isnan(prices[i])) << i;
        EXPECT_EQ(status[i].error().code(), expected.error().code()) << i;
        EXPECT_EQ(status[i].error().message(), expected.error().message()) << i;
      }
    }
  }
}

TEST(ResolvedAmericanPriceBatch, ValidatesEveryNonOwningSpan) {
  const std::vector<double> strikes{90.0, 100.0};
  const std::vector<double> one_sigma{0.2};
  const std::vector<Side> sides{Side::Put, Side::Call};
  std::vector<double> prices(2);
  std::vector<Status> status(2);
  const ResolvedAmericanPriceBatchRequest request{
      .S = 100.0,
      .T = 0.5,
      .r = 0.04,
      .q = 0.01,
      .K = strikes,
      .sigma = one_sigma,
      .side = sides,
      .method = AmericanMethod::AndersenLake,
      .al_opts = std::optional<AlOpts>{al_fast_opts()},
      .isa = simd::SimdIsa::Auto,
      .price = prices,
      .status = status,
      .pack_dispatch = {},
  };
  const Status result = american_price_batch_resolved(request);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(ResolvedAmericanPriceBatch, RejectsShiftedSigmaPriceOverlapBeforeWritesOrCounters) {
  const std::vector<double> strikes{90.0, 110.0};
  std::vector<double> sigma_and_price{0.20, 0.30, 777.0};
  const std::vector<double> before = sigma_and_price;
  const std::span<const double> sigma{sigma_and_price.data(), 2};
  const std::span<double> shifted_price{sigma_and_price.data() + 1, 2};
  const std::vector<Side> sides{Side::Put, Side::Call};
  std::vector<Status> status(2);
  std::vector<simd::SimdRoute> dispatch(2, simd::SimdRoute::Avx2);
  const ResolvedAmericanPriceBatchRequest request{
      .S = 100.0,
      .T = 0.73,
      .r = 0.04,
      .q = 0.06,
      .K = strikes,
      .sigma = sigma,
      .side = sides,
      .method = AmericanMethod::AndersenLake,
      .al_opts = std::optional<AlOpts>{al_fast_opts()},
      .isa = simd::SimdIsa::Auto,
      .price = shifted_price,
      .status = status,
      .pack_dispatch = dispatch,
  };

  if constexpr (counters::counters_enabled()) {
    counters::reset();
  }
  const Status result = american_price_batch_resolved(request);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(sigma_and_price, before);
  EXPECT_TRUE(status[0].has_value());
  EXPECT_TRUE(status[1].has_value());
  EXPECT_EQ(dispatch[0], simd::SimdRoute::Avx2);
  EXPECT_EQ(dispatch[1], simd::SimdRoute::Avx2);
  if constexpr (counters::counters_enabled()) {
    const counters::Snapshot snapshot = counters::snapshot();
    EXPECT_EQ(snapshot.get(counters::Counter::ResolvedPriceWrapperCalls), 0u);
    EXPECT_EQ(snapshot.get(counters::Counter::ResolvedPriceWrapperLanes), 0u);
  }
}

TEST(ResolvedAmericanPriceBatch, PreservesDistinctUnsupportedAndInvalidErrors) {
  IsaGuard guard;
  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
  const std::vector<double> strikes{100.0, -1.0};
  const std::vector<double> sigma{0.25, 0.25};
  const std::vector<Side> sides(2, Side::Put);
  std::vector<double> prices(2);
  std::vector<Status> status(2);
  std::vector<simd::SimdRoute> route(2);
  const ResolvedAmericanPriceBatchRequest request{
      .S = 100.0,
      .T = 1.0,
      .r = -0.01,
      .q = -0.05,
      .K = strikes,
      .sigma = sigma,
      .side = sides,
      .method = AmericanMethod::AndersenLake,
      .al_opts = std::optional<AlOpts>{al_fast_opts()},
      .isa = simd::SimdIsa::ForceAvx2,
      .price = prices,
      .status = status,
      .pack_dispatch = route,
  };

  ASSERT_TRUE(american_price_batch_resolved(request).has_value());
  ASSERT_FALSE(status[0].has_value());
  ASSERT_FALSE(status[1].has_value());
  EXPECT_EQ(status[0].error().code(), ErrorCode::NotImplemented);
  EXPECT_EQ(status[1].error().code(), ErrorCode::InvalidArgument);
  const Result<double> unsupported = american_price(
      100.0, strikes[0], 1.0, sigma[0], -0.01, -0.05, Side::Put,
      AmericanMethod::AndersenLake, std::optional<AlOpts>{al_fast_opts()});
  const Result<double> invalid = american_price(
      100.0, strikes[1], 1.0, sigma[1], -0.01, -0.05, Side::Put,
      AmericanMethod::AndersenLake, std::optional<AlOpts>{al_fast_opts()});
  ASSERT_FALSE(unsupported.has_value());
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(status[0].error().message(), unsupported.error().message());
  EXPECT_EQ(status[1].error().message(), invalid.error().message());
  EXPECT_TRUE(std::isnan(prices[0]));
  EXPECT_TRUE(std::isnan(prices[1]));
  EXPECT_EQ(route[0], simd::SimdRoute::Scalar);
  EXPECT_EQ(route[1], simd::SimdRoute::Scalar);
  EXPECT_EQ(simd::simd_isa_override(), simd::SimdIsa::ForceScalar);
}

TEST(ResolvedAmericanPriceBatch, NullOptionsHonorsLocalAvx2WithoutGlobalMutation) {
  IsaGuard guard;
  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
  const std::vector<double> strikes{82.0, 94.0, 106.0, 118.0};
  const std::vector<double> sigma{0.18, 0.24, 0.31, 0.42};
  const std::vector<Side> sides(4, Side::Put);
  std::vector<double> prices(4);
  std::vector<Status> status(4);
  std::vector<simd::SimdRoute> route(4);
  const ResolvedAmericanPriceBatchRequest request{
      .S = 100.0,
      .T = 0.73,
      .r = 0.05,
      .q = 0.01,
      .K = strikes,
      .sigma = sigma,
      .side = sides,
      .method = AmericanMethod::AndersenLake,
      .al_opts = std::nullopt,
      .isa = simd::SimdIsa::ForceAvx2,
      .price = prices,
      .status = status,
      .pack_dispatch = route,
  };

  if constexpr (counters::counters_enabled()) {
    counters::reset();
  }
  ASSERT_TRUE(american_price_batch_resolved(request).has_value());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const Result<double> expected = american_price(100.0, strikes[i], 0.73, sigma[i], 0.05, 0.01,
                                                   Side::Put, AmericanMethod::AndersenLake,
                                                   std::nullopt);
    ASSERT_TRUE(expected.has_value()) << i;
    EXPECT_TRUE(status[i].has_value()) << i;
    const simd::SimdRoute expected_dispatch =
        simd::have_avx2() ? simd::SimdRoute::Avx2
                          : simd::SimdRoute::Scalar;
    EXPECT_EQ(route[i], expected_dispatch) << i;
    if (expected_dispatch == simd::SimdRoute::Avx2) {
      EXPECT_LE(std::abs(prices[i] - *expected), 1.0e-3) << i;
    } else {
      EXPECT_EQ(prices[i], *expected) << i;
    }
  }
  EXPECT_EQ(simd::simd_isa_override(), simd::SimdIsa::ForceScalar);
  if constexpr (counters::counters_enabled()) {
    const counters::Snapshot snapshot = counters::snapshot();
    EXPECT_EQ(snapshot.get(counters::Counter::ResolvedPriceWrapperCalls), 1u);
    EXPECT_EQ(snapshot.get(counters::Counter::ResolvedPriceWrapperLanes), 4u);
    EXPECT_EQ(snapshot.get(counters::Counter::AmericanAvxPackDispatches),
              simd::have_avx2() ? 1u : 0u);
    EXPECT_EQ(snapshot.get(counters::Counter::AmericanWrapperKnownScalarLanes),
              simd::have_avx2() ? 0u : 4u);
  }
}

TEST(ResolvedAmericanPriceBatch, EmptyRequestIsNoOp) {
  const ResolvedAmericanPriceBatchRequest request{};
  EXPECT_TRUE(american_price_batch_resolved(request).has_value());
}

TEST(ResolvedAmericanPriceBatch, EligibleTailLaneRemainsExactScalar) {
  const std::vector<double> strikes{82.0, 94.0, 106.0, 118.0, 125.0};
  const std::vector<double> sigma(5, 0.25);
  const std::vector<Side> sides(5, Side::Put);
  std::vector<double> prices(5);
  std::vector<Status> status(5);
  std::vector<simd::SimdRoute> dispatch(5);
  const ResolvedAmericanPriceBatchRequest request{
      .S = 100.0,
      .T = 0.73,
      .r = 0.05,
      .q = 0.01,
      .K = strikes,
      .sigma = sigma,
      .side = sides,
      .method = AmericanMethod::AndersenLake,
      .al_opts = std::nullopt,
      .isa = simd::SimdIsa::ForceAvx2,
      .price = prices,
      .status = status,
      .pack_dispatch = dispatch,
  };

  ASSERT_TRUE(american_price_batch_resolved(request).has_value());
  const Result<double> expected = american_price(
      100.0, strikes.back(), 0.73, sigma.back(), 0.05, 0.01, Side::Put,
      AmericanMethod::AndersenLake, std::nullopt);
  ASSERT_TRUE(expected.has_value());
  EXPECT_TRUE(status.back().has_value());
  EXPECT_EQ(dispatch.back(), simd::SimdRoute::Scalar);
  EXPECT_EQ(prices.back(), *expected);
}

TEST(ResolvedAmericanPriceBatch, DegenerateLaneIsExactScalar) {
  const std::vector<double> strikes{105.0};
  const std::vector<double> sigma{0.0};
  const std::vector<Side> sides{Side::Put};
  std::vector<double> prices(1);
  std::vector<Status> status(1);
  std::vector<simd::SimdRoute> dispatch(1);
  const ResolvedAmericanPriceBatchRequest request{
      .S = 100.0,
      .T = 0.73,
      .r = 0.05,
      .q = 0.01,
      .K = strikes,
      .sigma = sigma,
      .side = sides,
      .method = AmericanMethod::AndersenLake,
      .al_opts = std::nullopt,
      .isa = simd::SimdIsa::ForceAvx2,
      .price = prices,
      .status = status,
      .pack_dispatch = dispatch,
  };

  ASSERT_TRUE(american_price_batch_resolved(request).has_value());
  const Result<double> expected = american_price(
      100.0, strikes[0], 0.73, sigma[0], 0.05, 0.01, Side::Put,
      AmericanMethod::AndersenLake, std::nullopt);
  ASSERT_TRUE(expected.has_value());
  EXPECT_TRUE(status[0].has_value());
  EXPECT_EQ(dispatch[0], simd::SimdRoute::Scalar);
  EXPECT_EQ(prices[0], *expected);
}

TEST(ResolvedAmericanPriceBatch, AutoWithEngagedOptionsIsExactScalar) {
  IsaGuard guard;
  simd::set_simd_isa_override(simd::SimdIsa::ForceAvx2);
  const std::vector<double> strikes{90.0, 110.0};
  const std::vector<double> sigma{0.20, 0.30};
  const std::vector<Side> sides{Side::Put, Side::Call};
  std::vector<double> prices(2);
  std::vector<Status> status(2);
  std::vector<simd::SimdRoute> dispatch(2);
  const ResolvedAmericanPriceBatchRequest request{
      .S = 100.0,
      .T = 0.73,
      .r = 0.04,
      .q = 0.06,
      .K = strikes,
      .sigma = sigma,
      .side = sides,
      .method = AmericanMethod::AndersenLake,
      .al_opts = std::optional<AlOpts>{al_fast_opts()},
      .isa = simd::SimdIsa::Auto,
      .price = prices,
      .status = status,
      .pack_dispatch = dispatch,
  };

  if constexpr (counters::counters_enabled()) {
    counters::reset();
  }
  ASSERT_TRUE(american_price_batch_resolved(request).has_value());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const Result<double> expected = american_price(
        100.0, strikes[i], 0.73, sigma[i], 0.04, 0.06, sides[i],
        AmericanMethod::AndersenLake, std::optional<AlOpts>{al_fast_opts()});
    ASSERT_TRUE(expected.has_value()) << i;
    EXPECT_EQ(dispatch[i], simd::SimdRoute::Scalar) << i;
    EXPECT_EQ(prices[i], *expected) << i;
  }
  EXPECT_EQ(simd::simd_isa_override(), simd::SimdIsa::ForceAvx2);
  if constexpr (counters::counters_enabled()) {
    const counters::Snapshot snapshot = counters::snapshot();
    EXPECT_EQ(snapshot.get(counters::Counter::ResolvedPriceWrapperCalls), 1u);
    EXPECT_EQ(snapshot.get(counters::Counter::ResolvedPriceWrapperLanes), 2u);
    EXPECT_EQ(snapshot.get(counters::Counter::AmericanAvxPackDispatches), 0u);
    EXPECT_EQ(snapshot.get(counters::Counter::AmericanWrapperKnownScalarLanes), 2u);
  }
}

// Configured resolved price batch.
struct ResolvedAlOptsCase {
  const char* name;
  AlOpts opts;
};

class ResolvedAmericanPriceBatchEngagedOpts
    : public ::testing::TestWithParam<ResolvedAlOptsCase> {};

TEST_P(ResolvedAmericanPriceBatchEngagedOpts,
       ForceAvx2PreservesPresetSidesErrorsDegeneratesAndTail) {
  IsaGuard guard;
  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);

  // The first four genuine American lanes form one complete pack. The invalid
  // and degenerate lanes patch through the exact scalar reference; the final
  // three eligible lanes are an incomplete tail and must also remain exact.
  const std::vector<double> strikes{82.0, 94.0, 106.0, 118.0, -1.0,
                                    105.0, 88.0, 101.0, 121.0};
  const std::vector<double> sigma{0.18, 0.24, 0.31, 0.42, 0.25,
                                  0.0,  0.22, 0.29, 0.37};
  const std::vector<Side> sides{Side::Put,  Side::Call, Side::Put,
                                Side::Call, Side::Put,  Side::Call,
                                Side::Put,  Side::Call, Side::Put};
  std::vector<double> prices(strikes.size());
  std::vector<Status> status(strikes.size());
  std::vector<simd::SimdRoute> dispatch(strikes.size());
  const std::optional<AlOpts> opts{GetParam().opts};
  const ResolvedAmericanPriceBatchRequest request{
      .S = 100.0,
      .T = 0.73,
      .r = 0.04,
      .q = 0.06,
      .K = strikes,
      .sigma = sigma,
      .side = sides,
      .method = AmericanMethod::AndersenLake,
      .al_opts = opts,
      .isa = simd::SimdIsa::ForceAvx2,
      .price = prices,
      .status = status,
      .pack_dispatch = dispatch,
  };

  ASSERT_TRUE(american_price_batch_resolved(request).has_value());
  EXPECT_EQ(simd::simd_isa_override(), simd::SimdIsa::ForceScalar);
  const simd::SimdRoute complete_pack_route =
      simd::have_avx2() ? simd::SimdRoute::Avx2 : simd::SimdRoute::Scalar;
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const Result<double> expected = american_price(100.0, strikes[i], 0.73, sigma[i],
                                                   0.04, 0.06, sides[i],
                                                   AmericanMethod::AndersenLake, opts);
    EXPECT_EQ(status[i].has_value(), expected.has_value()) << "lane=" << i;
    if (!expected.has_value()) {
      EXPECT_TRUE(std::isnan(prices[i])) << "lane=" << i;
      EXPECT_EQ(status[i].error().code(), expected.error().code()) << "lane=" << i;
      EXPECT_EQ(status[i].error().message(), expected.error().message()) << "lane=" << i;
      EXPECT_EQ(dispatch[i], simd::SimdRoute::Scalar) << "lane=" << i;
      continue;
    }
    if (i < 4) {
      EXPECT_EQ(dispatch[i], complete_pack_route) << "lane=" << i;
    } else {
      EXPECT_EQ(dispatch[i], simd::SimdRoute::Scalar) << "lane=" << i;
    }
    if (dispatch[i] == simd::SimdRoute::Avx2) {
      // Normal-regime lanes use the kernel's normal-grid immateriality contract.
      EXPECT_LE(std::abs(prices[i] - *expected), 1.0e-6) << "lane=" << i;
    } else {
      EXPECT_EQ(prices[i], *expected) << "lane=" << i;
    }
  }
}

TEST_P(ResolvedAmericanPriceBatchEngagedOpts,
       LowLevelKernelPreservesPresetOnInternalPatchAndTail) {
  const std::vector<double> spot(5, 100.0);
  const std::vector<double> strikes{82.0, 94.0, 106.0, 118.0, 125.0};
  const std::vector<double> tenor(5, 0.73);
  const std::vector<double> sigma{0.18, 0.24, 0.31, 0.0, 0.37};
  const std::vector<double> rate(5, 0.05);
  const std::vector<double> yield(5, 0.01);
  std::vector<double> prices(5);
  const std::optional<AlOpts> opts{GetParam().opts};

  const simd::SimdRoute route = simd::american_put_boundary_batch(
      spot.data(), strikes.data(), tenor.data(), sigma.data(), rate.data(), yield.data(),
      prices.data(), prices.size(), opts, simd::SimdIsa::ForceAvx2);
  EXPECT_EQ(route, simd::have_avx2() ? simd::SimdRoute::Avx2 : simd::SimdRoute::Scalar);
  for (std::size_t i = 0; i < prices.size(); ++i) {
    const Result<double> expected = andersen_lake(spot[i], strikes[i], tenor[i], sigma[i],
                                                 rate[i], yield[i], Side::Put, opts);
    ASSERT_TRUE(expected.has_value()) << "lane=" << i;
    if (route == simd::SimdRoute::Avx2 && i < 3) {
      EXPECT_LE(std::abs(prices[i] - *expected), 1.0e-6) << "lane=" << i;
    } else {
      // Lane 3 is an in-pack degenerate patch; lane 4 is the low-level tail.
      EXPECT_EQ(prices[i], *expected) << "lane=" << i;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    Presets, ResolvedAmericanPriceBatchEngagedOpts,
    ::testing::Values(ResolvedAlOptsCase{"explicit_default", al_default_opts()},
                      ResolvedAlOptsCase{"fast", al_fast_opts()},
                      ResolvedAlOptsCase{"custom", AlOpts{9, 32, 6, 3.0e-9}}),
    [](const ::testing::TestParamInfo<ResolvedAlOptsCase>& info) { return info.param.name; });

struct AlSchemeMappingCase {
  const char* name;
  AlOpts opts;
};

class AmericanBoundaryBatchSchemeMapping
    : public ::testing::TestWithParam<AlSchemeMappingCase> {};

void expect_resolved_grid_matches_force_scalar(const AlOpts& configured, bool stress) {
  const double S = 100.0;
  const double T = stress ? (1.0 / 365.0) : 0.73;
  const double r = stress ? 0.15 : 0.04;
  const double q = stress ? 0.18 : 0.06;
  const double gate = stress ? 1.0e-3 : 1.0e-6;
  const std::vector<double> strikes =
      stress ? std::vector<double>{20.0, 400.0, 25.0, 300.0, 95.0, 105.0, 100.0,
                                   110.0, -1.0, 105.0, 120.0, 250.0, 10.0}
             : std::vector<double>{75.0, 88.0, 96.0, 103.0, 112.0, 125.0, 82.0,
                                   118.0, -1.0, 105.0, 92.0, 108.0, 132.0};
  const std::vector<double> sigma =
      stress ? std::vector<double>{0.20, 0.20, 0.60, 0.60, 0.25, 0.25, 0.80,
                                   2.50, 0.20, 0.0, 0.30, 1.50, 0.02}
             : std::vector<double>{0.12, 0.18, 0.24, 0.31, 0.38, 0.45, 0.20,
                                   0.35, 0.25, 0.0, 0.22, 0.29, 0.41};
  const std::vector<Side> side{Side::Put,  Side::Call, Side::Put,  Side::Call,
                               Side::Put,  Side::Call, Side::Put,  Side::Call,
                               Side::Put,  Side::Call, Side::Put,  Side::Call,
                               Side::Put};
  std::vector<double> scalar_price(strikes.size());
  std::vector<double> avx_price(strikes.size());
  std::vector<Status> scalar_status(strikes.size());
  std::vector<Status> avx_status(strikes.size());
  std::vector<simd::SimdRoute> scalar_dispatch(strikes.size());
  std::vector<simd::SimdRoute> avx_dispatch(strikes.size());
  const std::optional<AlOpts> opts{configured};
  const ResolvedAmericanPriceBatchRequest scalar_request{
      .S = S,
      .T = T,
      .r = r,
      .q = q,
      .K = strikes,
      .sigma = sigma,
      .side = side,
      .method = AmericanMethod::AndersenLake,
      .al_opts = opts,
      .isa = simd::SimdIsa::ForceScalar,
      .price = scalar_price,
      .status = scalar_status,
      .pack_dispatch = scalar_dispatch,
  };
  const ResolvedAmericanPriceBatchRequest avx_request{
      .S = S,
      .T = T,
      .r = r,
      .q = q,
      .K = strikes,
      .sigma = sigma,
      .side = side,
      .method = AmericanMethod::AndersenLake,
      .al_opts = opts,
      .isa = simd::SimdIsa::ForceAvx2,
      .price = avx_price,
      .status = avx_status,
      .pack_dispatch = avx_dispatch,
  };

  ASSERT_TRUE(american_price_batch_resolved(scalar_request).has_value());
  ASSERT_TRUE(american_price_batch_resolved(avx_request).has_value());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    SCOPED_TRACE(::testing::Message() << "stress=" << stress << " lane=" << i);
    EXPECT_EQ(scalar_dispatch[i], simd::SimdRoute::Scalar);
    EXPECT_EQ(avx_dispatch[i], i < 8 ? simd::SimdRoute::Avx2
                                    : simd::SimdRoute::Scalar);
    EXPECT_EQ(avx_status[i].has_value(), scalar_status[i].has_value());
    if (!scalar_status[i].has_value()) {
      EXPECT_TRUE(std::isnan(avx_price[i]));
      EXPECT_EQ(avx_status[i].error().code(), scalar_status[i].error().code());
      EXPECT_EQ(avx_status[i].error().message(), scalar_status[i].error().message());
      continue;
    }
    EXPECT_LE(std::abs(avx_price[i] - scalar_price[i]), gate);
  }
}

void expect_low_level_patch_and_tail_match_force_scalar(const AlOpts& configured,
                                                        bool stress) {
  const std::vector<double> spot(9, 100.0);
  const std::vector<double> strikes =
      stress ? std::vector<double>{20.0, 400.0, 25.0, 105.0, 95.0,
                                   300.0, 100.0, 110.0, 250.0}
             : std::vector<double>{75.0, 88.0, 103.0, 105.0, 112.0,
                                   125.0, 82.0, 118.0, 132.0};
  const std::vector<double> tenor(9, stress ? (1.0 / 365.0) : 0.73);
  const std::vector<double> sigma =
      stress ? std::vector<double>{0.20, 0.20, 0.60, 0.0, 0.02,
                                   2.50, 0.80, 1.50, 0.30}
             : std::vector<double>{0.12, 0.18, 0.31, 0.0, 0.38,
                                   0.45, 0.20, 0.35, 0.41};
  const std::vector<double> rate(9, stress ? 0.15 : 0.05);
  const std::vector<double> yield(9, stress ? 0.18 : 0.01);
  std::vector<double> scalar_price(9);
  std::vector<double> avx_price(9);
  const std::optional<AlOpts> opts{configured};
  const simd::SimdRoute scalar_route = simd::american_put_boundary_batch(
      spot.data(), strikes.data(), tenor.data(), sigma.data(), rate.data(), yield.data(),
      scalar_price.data(), scalar_price.size(), opts, simd::SimdIsa::ForceScalar);
  const simd::SimdRoute avx_route = simd::american_put_boundary_batch(
      spot.data(), strikes.data(), tenor.data(), sigma.data(), rate.data(), yield.data(),
      avx_price.data(), avx_price.size(), opts, simd::SimdIsa::ForceAvx2);
  EXPECT_EQ(scalar_route, simd::SimdRoute::Scalar);
  EXPECT_EQ(avx_route, simd::SimdRoute::Avx2);
  const double gate = stress ? 1.0e-3 : 1.0e-6;
  for (std::size_t i = 0; i < scalar_price.size(); ++i) {
    SCOPED_TRACE(::testing::Message() << "stress=" << stress << " lane=" << i);
    if (i == 3 || i == 8) {
      // Lane 3 patches from inside a complete pack; lane 8 is the scalar tail.
      EXPECT_EQ(avx_price[i], scalar_price[i]);
    } else {
      EXPECT_LE(std::abs(avx_price[i] - scalar_price[i]), gate);
    }
  }
}

TEST_P(AmericanBoundaryBatchSchemeMapping, NormalAndStressGridsMatchForceScalar) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on this host";
  }
  expect_resolved_grid_matches_force_scalar(GetParam().opts, false);
  expect_resolved_grid_matches_force_scalar(GetParam().opts, true);
  expect_low_level_patch_and_tail_match_force_scalar(GetParam().opts, false);
  expect_low_level_patch_and_tail_match_force_scalar(GetParam().opts, true);
}

INSTANTIATE_TEST_SUITE_P(
    SchemeBoundaries, AmericanBoundaryBatchSchemeMapping,
    ::testing::Values(
        AlSchemeMappingCase{"n6_q8_i1_tight", AlOpts{6, 8, 1, 1.0e-14}},
        AlSchemeMappingCase{"n7_q16_i32_loose", AlOpts{7, 16, 32, 1.0e-4}},
        AlSchemeMappingCase{"n12_q24_i1_loose", AlOpts{12, 24, 1, 1.0e-4}},
        AlSchemeMappingCase{"n16_q32_i32_tight", AlOpts{16, 32, 32, 1.0e-14}},
        AlSchemeMappingCase{"n6_q48_i32_mid", AlOpts{6, 48, 32, 1.0e-9}},
        AlSchemeMappingCase{"n7_q64_i1_mid", AlOpts{7, 64, 1, 1.0e-9}},
        AlSchemeMappingCase{"n12_q8_i32_tight", AlOpts{12, 8, 32, 1.0e-14}},
        AlSchemeMappingCase{"n16_q16_i1_loose", AlOpts{16, 16, 1, 1.0e-4}},
        AlSchemeMappingCase{"n6_q24_i1_mid", AlOpts{6, 24, 1, 1.0e-9}},
        AlSchemeMappingCase{"n7_q32_i32_tight", AlOpts{7, 32, 32, 1.0e-14}},
        AlSchemeMappingCase{"n12_q48_i1_loose", AlOpts{12, 48, 1, 1.0e-4}},
        AlSchemeMappingCase{"n16_q64_i32_mid", AlOpts{16, 64, 32, 1.0e-9}},
        // K2 ql_fast marks rung: nb=7, fp=8, 2 sweeps, DECOUPLED premium p=32. The
        // AVX2 batch's intended ship tier — a low sweep budget makes it the most
        // seed-sensitive production scheme, so it pins the 4-wide BAW seed's economic
        // parity vs the specialized scalar (7,8) baseline (kernel-stage1.md).
        AlSchemeMappingCase{"n7_q8_i2_p32_qlfast", AlOpts{7, 8, 2, 1.0e-8, 32}}),
    [](const ::testing::TestParamInfo<AlSchemeMappingCase>& info) {
      return info.param.name;
    });

// ── K3: laned analytic PUT Greeks bundle (american_put_greeks_batch, AVX2) ────
//
// The laned kernel solves the 5 analytic boundaries (base, sigma+/-, r+/-) 4-wide per
// pack and re-prices the spot stencils, matching scalar american_greeks_al. Parity:
//  * ForceScalar route == american_greeks_al per contract, BIT-identical (it IS the
//    scalar oracle, no AVX2 path);
//  * ForceAvx2 route == american_greeks_al within the economic gate — the only
//    difference is the AVX2 transcendentals in the 13 stencil prices (~1e-13 USD),
//    amplified by the FD denominators (documented per-greek below).
TEST(AmericanPutGreeksBatchAvx2, MatchesScalarAl) {
  // Genuine early-exercise American puts (r>0), a moneyness x maturity x vol grid.
  struct C { double S, K, T, sigma, r, q; };
  std::vector<C> g;
  const double S = 100.0;
  for (double m : {0.80, 0.90, 0.95, 1.00, 1.05, 1.10, 1.20}) {
    for (double t : {0.08, 0.25, 0.75, 2.0}) {
      for (double v : {0.15, 0.25, 0.40}) {
        for (double r : {0.03, 0.06}) {
          g.push_back(C{S, S / m, t, v, r, 0.01});
        }
      }
    }
  }
  const std::size_t n = g.size();
  std::vector<double> vS(n), vK(n), vT(n), vsig(n), vr(n), vq(n);
  for (std::size_t i = 0; i < n; ++i) {
    vS[i] = g[i].S; vK[i] = g[i].K; vT[i] = g[i].T;
    vsig[i] = g[i].sigma; vr[i] = g[i].r; vq[i] = g[i].q;
  }

  // ForceScalar route == american_greeks_al, bit-identical.
  std::vector<AmericanGreeks> scl(n);
  const simd::SimdRoute sroute = simd::american_put_greeks_batch(
      vS.data(), vK.data(), vT.data(), vsig.data(), vr.data(), vq.data(), n,
      std::nullopt, scl.data(), simd::SimdIsa::ForceScalar);
  EXPECT_EQ(sroute, simd::SimdRoute::Scalar);
  for (std::size_t i = 0; i < n; ++i) {
    const auto al = american_greeks_al(vS[i], vK[i], vT[i], vsig[i], vr[i], vq[i], Side::Put);
    ASSERT_TRUE(al.has_value()) << "i=" << i;
    EXPECT_EQ(scl[i], *al) << "ForceScalar must equal american_greeks_al bit-for-bit, i=" << i;
  }

  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on host (scalar parity checked above)";
  }

  // ForceAvx2 route == american_greeks_al within the economic gate.
  std::vector<AmericanGreeks> avx(n);
  const simd::SimdRoute aroute = simd::american_put_greeks_batch(
      vS.data(), vK.data(), vT.data(), vsig.data(), vr.data(), vq.data(), n,
      std::nullopt, avx.data(), simd::SimdIsa::ForceAvx2);
  EXPECT_EQ(aroute, simd::SimdRoute::Avx2);

  auto reld = [](double a, double b, double floor) {
    return std::abs(a - b) / std::max({std::abs(a), std::abs(b), floor});
  };
  double mp = 0, md = 0, mg = 0, mv = 0, mvl = 0, mr = 0, mvn = 0, mt = 0, mc = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const AmericanGreeks a = avx[i], s = scl[i];
    mp = std::max(mp, reld(a.price, s.price, 1e-6));
    md = std::max(md, reld(a.delta, s.delta, 1e-6));
    mg = std::max(mg, reld(a.gamma, s.gamma, 1e-6));
    mv = std::max(mv, reld(a.vega, s.vega, 1e-4));
    mvl = std::max(mvl, reld(a.volga, s.volga, 1e-3));
    mr = std::max(mr, reld(a.rho, s.rho, 1e-4));
    mvn = std::max(mvn, reld(a.vanna, s.vanna, 1e-5));
    mt = std::max(mt, reld(a.theta, s.theta, 1e-4));
    mc = std::max(mc, reld(a.charm, s.charm, 1e-4));
  }
  std::printf("[K3 laned-greeks parity] rel-dev vs american_greeks_al (n=%zu): "
              "price=%.2e delta=%.2e gamma=%.2e vega=%.2e volga=%.2e rho=%.2e "
              "vanna=%.2e theta=%.2e charm=%.2e\n",
              n, mp, md, mg, mv, mvl, mr, mvn, mt, mc);
  // Economic gate (measured deviations far under; all are the AVX2-transcendental
  // ~1e-13 USD price delta amplified by the FD denominators, economically negligible —
  // e.g. a 3e-5 RELATIVE charm move). price/delta ride one boundary's spot stencils so
  // stay near machine; gamma/volga (÷h^2) and charm (÷h^3 speed) carry the most amp.
  EXPECT_LT(mp, 1e-9) << "price";    // measured ~1.6e-10
  EXPECT_LT(md, 1e-8) << "delta";    // measured ~5.7e-9
  EXPECT_LT(mg, 1e-5) << "gamma";    // measured ~2.1e-6
  EXPECT_LT(mv, 1e-7) << "vega";     // measured ~1.4e-8
  EXPECT_LT(mvl, 1e-4) << "volga";   // measured ~4.7e-6
  EXPECT_LT(mr, 1e-6) << "rho";      // measured ~3.1e-7
  EXPECT_LT(mvn, 1e-5) << "vanna";   // measured ~5.6e-7
  EXPECT_LT(mt, 1e-5) << "theta";    // measured ~1.5e-7
  EXPECT_LT(mc, 1e-4) << "charm";    // measured ~3.0e-5
}

// WS-K stress-corner parity: the laned analytic PUT greeks bundle vs scalar
// american_greeks_al on the HARD corners the flip must survive — deep ITM/OTM wings,
// near-expiry (down to 1/365), low vol (0.05) and high vol (1.20), and higher rates.
// Genuine early-exercise lanes ride the AVX2 kernel; corners that fall out of the
// American regime or error are patched to (and asserted against) the exact scalar oracle,
// so the comparison holds within the economic gate on every finite lane. This closes the
// "grid incl. deep wings, near-expiry, hi/lo vol" requirement for greeks (marks already
// have normal_grid + stress_grid in simd_american_test.cpp).
TEST(AmericanPutGreeksBatchAvx2, MatchesScalarAl_StressCorners) {
  struct C { double S, K, T, sigma, r, q; };
  std::vector<C> g;
  const double S = 100.0;
  // Deep wings x near-to-long expiry x lo/hi vol x higher rates. Puts, r>0.
  for (double K : {40.0, 55.0, 70.0, 130.0, 160.0, 200.0}) {   // deep OTM..deep ITM puts
    for (double t : {1.0 / 365.0, 3.0 / 365.0, 0.02, 0.10, 1.0, 3.0}) { // incl. near-expiry
      for (double v : {0.05, 0.08, 0.60, 1.20}) {              // lo + hi vol
        for (double r : {0.02, 0.08, 0.12}) {
          g.push_back(C{S, K, t, v, r, 0.0});
        }
      }
    }
  }
  const std::size_t n = g.size();
  std::vector<double> vS(n), vK(n), vT(n), vsig(n), vr(n), vq(n);
  for (std::size_t i = 0; i < n; ++i) {
    vS[i] = g[i].S; vK[i] = g[i].K; vT[i] = g[i].T;
    vsig[i] = g[i].sigma; vr[i] = g[i].r; vq[i] = g[i].q;
  }

  // ForceScalar route == american_greeks_al, bit-identical (finite lanes).
  std::vector<AmericanGreeks> scl(n);
  const simd::SimdRoute sroute = simd::american_put_greeks_batch(
      vS.data(), vK.data(), vT.data(), vsig.data(), vr.data(), vq.data(), n,
      std::nullopt, scl.data(), simd::SimdIsa::ForceScalar);
  EXPECT_EQ(sroute, simd::SimdRoute::Scalar);
  for (std::size_t i = 0; i < n; ++i) {
    const auto al = american_greeks_al(vS[i], vK[i], vT[i], vsig[i], vr[i], vq[i], Side::Put);
    if (!al.has_value()) {
      continue; // corner the scalar oracle itself rejects; batch NaN-fills to match
    }
    EXPECT_EQ(scl[i], *al) << "ForceScalar must equal american_greeks_al bit-for-bit, i=" << i;
  }

  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on host (scalar parity checked above)";
  }

  // ForceAvx2 route == american_greeks_al within the economic gate on every finite lane.
  std::vector<AmericanGreeks> avx(n);
  const simd::SimdRoute aroute = simd::american_put_greeks_batch(
      vS.data(), vK.data(), vT.data(), vsig.data(), vr.data(), vq.data(), n,
      std::nullopt, avx.data(), simd::SimdIsa::ForceAvx2);
  EXPECT_EQ(aroute, simd::SimdRoute::Avx2);

  auto reld = [](double a, double b, double floor) {
    return std::abs(a - b) / std::max({std::abs(a), std::abs(b), floor});
  };
  double mp = 0, md = 0, mg = 0, mv = 0, mvl = 0, mr = 0, mvn = 0, mt = 0, mc = 0;
  std::size_t n_cmp = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const AmericanGreeks a = avx[i], s = scl[i];
    if (!std::isfinite(s.price) || !std::isfinite(a.price)) {
      continue; // patched/rejected corner; scalar-parity handled above
    }
    ++n_cmp;
    mp = std::max(mp, reld(a.price, s.price, 1e-6));
    md = std::max(md, reld(a.delta, s.delta, 1e-6));
    mg = std::max(mg, reld(a.gamma, s.gamma, 1e-6));
    mv = std::max(mv, reld(a.vega, s.vega, 1e-4));
    mvl = std::max(mvl, reld(a.volga, s.volga, 1e-3));
    mr = std::max(mr, reld(a.rho, s.rho, 1e-4));
    mvn = std::max(mvn, reld(a.vanna, s.vanna, 1e-5));
    mt = std::max(mt, reld(a.theta, s.theta, 1e-4));
    mc = std::max(mc, reld(a.charm, s.charm, 1e-4));
  }
  std::printf("[K3 laned-greeks STRESS parity] rel-dev vs american_greeks_al "
              "(n_cmp=%zu/%zu): price=%.2e delta=%.2e gamma=%.2e vega=%.2e volga=%.2e "
              "rho=%.2e vanna=%.2e theta=%.2e charm=%.2e\n",
              n_cmp, n, mp, md, mg, mv, mvl, mr, mvn, mt, mc);
  EXPECT_GT(n_cmp, 0u); // at least some corners ride the vector kernel
  // Stress-corner economic gate. Measured on this dev box (n_cmp=432): price 7.6e-9,
  // delta 4.9e-8, gamma 2.8e-6, vega 1.4e-7, volga 2.8e-5, rho 1.4e-6, vanna 1.1e-5,
  // theta 1.2e-4, charm 5.7e-4 — all the AVX2-transcendental ~1e-13 USD price delta
  // amplified by the FD denominators (near-expiry drives ÷h^2/÷h^3 hardest; theta/charm
  // ride the continuation-region PDE at 1/365). Gates are set with headroom over measured
  // for cross-host robustness, still orders under any order-1 formula/sign bug.
  EXPECT_LT(mp, 5e-8) << "price";
  EXPECT_LT(md, 1e-7) << "delta";
  EXPECT_LT(mg, 1e-4) << "gamma";
  EXPECT_LT(mv, 1e-6) << "vega";
  EXPECT_LT(mvl, 1e-3) << "volga";
  EXPECT_LT(mr, 1e-5) << "rho";
  EXPECT_LT(mvn, 1e-4) << "vanna";
  EXPECT_LT(mt, 3e-4) << "theta";
  EXPECT_LT(mc, 2e-3) << "charm";
}

// A9 (simd-review finding 9): (1) the vector r-hr>0 eligibility is now conditional
// on need_rho, so a delta-only bundle with 0 < r <= hr stays on the vector path
// instead of needlessly patching; (2) the scalar patch path zeroes the unrequested
// greek columns (american_greeks_al can itself route to american_greeks_fd, which
// ignores the needs mask and fills the full bundle), so the laned bundle is
// internally consistent — EVERY lane, vector-handled OR scalar-patched, leaves the
// unrequested greeks at 0.
TEST(AmericanPutGreeksBatchAvx2, DeltaOnlyZeroesUnrequestedAcrossHandledAndPatchedLanes) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on host";
  }
  // Lane 0: r=0.05 American put (r>hr, vector-handled).
  // Lane 1: r=5e-5 American put (r<hr; with need_rho=false, now vector-handled).
  // Lane 2: European put r=-0.01, q=0.02 (not American -> scalar patch via al->fd,
  //         which fills the FULL bundle -> the RED-before lane).
  const std::vector<double> S = {100, 100, 100};
  const std::vector<double> K = {100, 100, 100};
  const std::vector<double> T = {0.5, 0.5, 0.5};
  const std::vector<double> sig = {0.25, 0.25, 0.25};
  const std::vector<double> r = {0.05, 5.0e-5, -0.01};
  const std::vector<double> q = {0.01, 0.01, 0.02};
  const std::size_t n = S.size();
  std::vector<AmericanGreeks> g(n);
  const simd::SimdRoute route = simd::american_put_greeks_batch(
      S.data(), K.data(), T.data(), sig.data(), r.data(), q.data(), n, std::nullopt, g.data(),
      simd::SimdIsa::ForceAvx2, /*need_vega=*/false, /*need_rho=*/false, /*need_charm=*/false);
  EXPECT_EQ(route, simd::SimdRoute::Avx2);
  for (std::size_t i = 0; i < n; ++i) {
    // Requested columns finite (all three lanes are priceable at their base).
    EXPECT_TRUE(std::isfinite(g[i].price)) << "i=" << i;
    EXPECT_TRUE(std::isfinite(g[i].delta)) << "i=" << i;
    EXPECT_TRUE(std::isfinite(g[i].gamma)) << "i=" << i;
    EXPECT_TRUE(std::isfinite(g[i].theta)) << "i=" << i;
    // Unrequested columns are exactly 0 in EVERY lane (handled and patched).
    EXPECT_EQ(g[i].vega, 0.0) << "i=" << i;
    EXPECT_EQ(g[i].volga, 0.0) << "i=" << i;
    EXPECT_EQ(g[i].vanna, 0.0) << "i=" << i;
    EXPECT_EQ(g[i].rho, 0.0) << "i=" << i;
    EXPECT_EQ(g[i].charm, 0.0) << "i=" << i;
  }
  // The r=5e-5 delta-only lane succeeds and matches the scalar analytic delta.
  const auto ref =
      american_greeks_al(S[1], K[1], T[1], sig[1], r[1], q[1], Side::Put, std::nullopt,
                         /*need_vega=*/false, /*need_rho=*/false, /*need_charm=*/false);
  ASSERT_TRUE(ref.has_value());
  EXPECT_NEAR(g[1].delta, ref->delta, 1e-6);
}

// ── P1b: laned analytic CALL Greeks bundle (american_call_greeks_batch, AVX2) ──
//
// Call-native mirror of the K3 put parity gate. Under McDonald-Schroder C(S,K,r,q)=
// P(K,S,q,r) the kernel solves the internal put (rate=q, yield=r, internal-strike=S) and
// prices the call spot stencils by strike homogeneity. Parity:
//  * ForceScalar route == american_greeks_al(Side::Call), BIT-identical (it IS the oracle);
//  * ForceAvx2 route == american_greeks_al(Side::Call) within the economic gate — the AVX2
//    transcendentals in the stencil prices + the ~1 ULP xmax strike-rescale, amplified by
//    the FD denominators (documented per-greek below).
TEST(AmericanCallGreeksBatchAvx2, MatchesScalarAl) {
  // Genuine early-exercise American CALLS (q>0 drives early exercise), moneyness x
  // maturity x vol x yield grid.
  struct C { double S, K, T, sigma, r, q; };
  std::vector<C> g;
  const double S = 100.0;
  for (double m : {0.80, 0.90, 0.95, 1.00, 1.05, 1.10, 1.20}) {
    for (double t : {0.08, 0.25, 0.75, 2.0}) {
      for (double v : {0.15, 0.25, 0.40}) {
        for (double qy : {0.03, 0.06}) {
          g.push_back(C{S, S / m, t, v, 0.01, qy});
        }
      }
    }
  }
  const std::size_t n = g.size();
  std::vector<double> vS(n), vK(n), vT(n), vsig(n), vr(n), vq(n);
  for (std::size_t i = 0; i < n; ++i) {
    vS[i] = g[i].S; vK[i] = g[i].K; vT[i] = g[i].T;
    vsig[i] = g[i].sigma; vr[i] = g[i].r; vq[i] = g[i].q;
  }

  // ForceScalar route == american_greeks_al(Side::Call), bit-identical.
  std::vector<AmericanGreeks> scl(n);
  const simd::SimdRoute sroute = simd::american_call_greeks_batch(
      vS.data(), vK.data(), vT.data(), vsig.data(), vr.data(), vq.data(), n,
      std::nullopt, scl.data(), simd::SimdIsa::ForceScalar);
  EXPECT_EQ(sroute, simd::SimdRoute::Scalar);
  for (std::size_t i = 0; i < n; ++i) {
    const auto al = american_greeks_al(vS[i], vK[i], vT[i], vsig[i], vr[i], vq[i], Side::Call);
    ASSERT_TRUE(al.has_value()) << "i=" << i;
    EXPECT_EQ(scl[i], *al) << "ForceScalar must equal american_greeks_al(Call) bit-for-bit, i=" << i;
  }

  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on host (scalar parity checked above)";
  }

  // ForceAvx2 route == american_greeks_al(Side::Call) within the economic gate.
  std::vector<AmericanGreeks> avx(n);
  const simd::SimdRoute aroute = simd::american_call_greeks_batch(
      vS.data(), vK.data(), vT.data(), vsig.data(), vr.data(), vq.data(), n,
      std::nullopt, avx.data(), simd::SimdIsa::ForceAvx2);
  EXPECT_EQ(aroute, simd::SimdRoute::Avx2);

  auto reld = [](double a, double b, double floor) {
    return std::abs(a - b) / std::max({std::abs(a), std::abs(b), floor});
  };
  double mp = 0, md = 0, mg = 0, mv = 0, mvl = 0, mr = 0, mvn = 0, mt = 0, mc = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const AmericanGreeks a = avx[i], s = scl[i];
    mp = std::max(mp, reld(a.price, s.price, 1e-6));
    md = std::max(md, reld(a.delta, s.delta, 1e-6));
    mg = std::max(mg, reld(a.gamma, s.gamma, 1e-6));
    mv = std::max(mv, reld(a.vega, s.vega, 1e-4));
    mvl = std::max(mvl, reld(a.volga, s.volga, 1e-3));
    mr = std::max(mr, reld(a.rho, s.rho, 1e-4));
    mvn = std::max(mvn, reld(a.vanna, s.vanna, 1e-5));
    mt = std::max(mt, reld(a.theta, s.theta, 1e-4));
    mc = std::max(mc, reld(a.charm, s.charm, 1e-4));
  }
  std::printf("[P1b laned-call-greeks parity] rel-dev vs american_greeks_al(Call) (n=%zu): "
              "price=%.2e delta=%.2e gamma=%.2e vega=%.2e volga=%.2e rho=%.2e "
              "vanna=%.2e theta=%.2e charm=%.2e\n",
              n, mp, md, mg, mv, mvl, mr, mvn, mt, mc);
  // Economic gate. Measured on this dev box (n=168): price 3.5e-9, delta 7.9e-8,
  // gamma 2.0e-6, vega 7.1e-8, volga 4.7e-6, rho 6.0e-7, vanna 1.4e-6, theta 2.2e-6,
  // charm 1.3e-5 — all RELATIVE. These sit ~1 order above the put kernel's (price 1.6e-10,
  // delta 5.7e-9) because the call path carries the McDonald-Schroder strike-homogeneity
  // rescale (xmax(S2)=XMAX·S2/S, ~1 ULP) and prices at spot=K/strike=S2 rather than the
  // scalar's operand ordering — on top of the shared AVX2 stencil transcendentals, all
  // amplified by the FD denominators. Economically nil: the worst column is a 1.3e-5
  // RELATIVE charm move and price agrees to 3.5e-9 relative, 10+ orders below a tick.
  // Gates set with headroom over measured for cross-host robustness, still orders under
  // any order-1 formula/sign bug.
  EXPECT_LT(mp, 5e-8) << "price";
  EXPECT_LT(md, 1e-6) << "delta";
  EXPECT_LT(mg, 1e-5) << "gamma";
  EXPECT_LT(mv, 1e-6) << "vega";
  EXPECT_LT(mvl, 1e-4) << "volga";
  EXPECT_LT(mr, 1e-5) << "rho";
  EXPECT_LT(mvn, 1e-5) << "vanna";
  EXPECT_LT(mt, 5e-5) << "theta";
  EXPECT_LT(mc, 2e-4) << "charm";
}

// FIX-1 / F1 (rev-ws-g M1-1): the A9 unrequested-column zeroing must apply to the CALL
// batch's scalar-patch path exactly as it does to the put batch's.
//
// The merge that produced this file's trunk took the A9 zeroing from one parent (which had
// only the put wrapper) and the call wrapper from the other (which had no zeroing), so the
// union lost the pairing. Consequence on a live path: solve_pnl_uniques sets
// base_needs.rho = (dr != 0.0), so on an ordinary no-rate-shift P&L step an AVX2-handled
// call lane returns rho == 0 while a scalar-patched call lane returns the FD oracle's fully
// populated rho — the same output column carrying an ISA- and lane-dependent value.
//
// The batch below straddles the patch boundary deliberately: n=3 is not a multiple of the
// 4-wide pack, and lane 1 carries q <= 0, which fails the call kernel's American-regime
// eligibility (classify_regime(q, r)) and is therefore serviced by the scalar oracle while
// lanes 0 and 2 are vector-handled.
TEST(AmericanCallGreeksBatchAvx2, DeltaOnlyZeroesUnrequestedAcrossHandledAndPatchedLanes) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on host";
  }
  // Lane 0: q=0.06 > 0 -> American call, vector-handled.
  // Lane 1: q=-0.01 <= 0 -> not American -> scalar patch (the RED-before lane).
  // Lane 2: q=0.03 > 0 -> American call, vector-handled.
  const std::vector<double> S = {100, 100, 100};
  const std::vector<double> K = {100, 100, 100};
  const std::vector<double> T = {0.5, 0.5, 0.5};
  const std::vector<double> sig = {0.25, 0.25, 0.25};
  const std::vector<double> r = {0.01, 0.02, 0.01};
  const std::vector<double> q = {0.06, -0.01, 0.03};
  const std::size_t n = S.size();

  // Anti-vacuity: request the FULL bundle first and prove the patched lane genuinely
  // carries nonzero vega/rho/charm. Without this the "== 0.0" assertions below could pass
  // simply because the lane's greeks happen to vanish.
  std::vector<AmericanGreeks> full(n);
  const simd::SimdRoute froute = simd::american_call_greeks_batch(
      S.data(), K.data(), T.data(), sig.data(), r.data(), q.data(), n, std::nullopt,
      full.data(), simd::SimdIsa::ForceAvx2, /*need_vega=*/true, /*need_rho=*/true,
      /*need_charm=*/true);
  EXPECT_EQ(froute, simd::SimdRoute::Avx2);
  EXPECT_NE(full[1].vega, 0.0) << "patched lane must have a nonzero vega when requested";
  EXPECT_NE(full[1].rho, 0.0) << "patched lane must have a nonzero rho when requested";
  EXPECT_NE(full[1].charm, 0.0) << "patched lane must have a nonzero charm when requested";

  // Delta-only request: no vega, no rho, no charm.
  std::vector<AmericanGreeks> g(n);
  const simd::SimdRoute route = simd::american_call_greeks_batch(
      S.data(), K.data(), T.data(), sig.data(), r.data(), q.data(), n, std::nullopt, g.data(),
      simd::SimdIsa::ForceAvx2, /*need_vega=*/false, /*need_rho=*/false, /*need_charm=*/false);
  EXPECT_EQ(route, simd::SimdRoute::Avx2);
  for (std::size_t i = 0; i < n; ++i) {
    // Requested columns finite in every lane (all three are priceable at their base).
    EXPECT_TRUE(std::isfinite(g[i].price)) << "i=" << i;
    EXPECT_TRUE(std::isfinite(g[i].delta)) << "i=" << i;
    EXPECT_TRUE(std::isfinite(g[i].gamma)) << "i=" << i;
    EXPECT_TRUE(std::isfinite(g[i].theta)) << "i=" << i;
    // Unrequested columns are exactly 0 in EVERY lane (vector-handled AND scalar-patched).
    EXPECT_EQ(g[i].vega, 0.0) << "i=" << i;
    EXPECT_EQ(g[i].volga, 0.0) << "i=" << i;
    EXPECT_EQ(g[i].vanna, 0.0) << "i=" << i;
    EXPECT_EQ(g[i].rho, 0.0) << "i=" << i;
    EXPECT_EQ(g[i].charm, 0.0) << "i=" << i;
  }
  // The scalar-patched lane still serves the REQUESTED columns from the scalar oracle.
  const auto ref = american_greeks_al(S[1], K[1], T[1], sig[1], r[1], q[1], Side::Call,
                                      std::nullopt);
  ASSERT_TRUE(ref.has_value());
  EXPECT_NEAR(g[1].delta, ref->delta, 1e-12);
  EXPECT_NEAR(g[1].price, ref->price, 1e-12);
}

// Greeks batch: bit-identical to per-contract american_greeks_fd.
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

// american_greeks_batch SoA surface routes analytic PUT lanes through the K3 laned
// bundle when AVX2 is selected; CALL lanes + FD route stay scalar. ForceAvx2(analytic)
// must match ForceScalar(analytic) within the economic gate (puts) / bit (calls).
TEST(AmericanGreeksBatchLaned, AnalyticAvx2MatchesScalar) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on host";
  }
  const Book b = make_american_greeks_book(); // mixed puts + American calls
  const std::size_t n = b.size();
  auto run = [&](simd::SimdIsa isa, std::vector<double>& dl, std::vector<double>& gm,
                 std::vector<double>& vg, std::vector<double>& th, std::vector<double>& rh,
                 std::vector<double>& vn, std::vector<double>& vl, std::vector<double>& cm,
                 std::vector<double>& px) {
    dl.assign(n, 0); gm.assign(n, 0); vg.assign(n, 0); th.assign(n, 0); rh.assign(n, 0);
    vn.assign(n, 0); vl.assign(n, 0); cm.assign(n, 0); px.assign(n, 0);
    simd::GreeksBatchSoA out{dl.data(), gm.data(), vg.data(), th.data(), rh.data(),
                             vn.data(), vl.data(), cm.data(), px.data()};
    PricingKernel kernel;
    kernel.analytic_greeks = true;
    kernel.isa = isa;
    PricingWorkspace ws;
    ASSERT_TRUE(american_greeks_batch(b.view(), GreekFieldMask::All, out, kernel, ws).has_value());
  };
  std::vector<double> ad, ag, av_, at, ar, avn, avl, ac, ap;
  std::vector<double> sd, sg, sv, st, sr, svn, svl, sc, sp;
  run(simd::SimdIsa::ForceAvx2, ad, ag, av_, at, ar, avn, avl, ac, ap);
  run(simd::SimdIsa::ForceScalar, sd, sg, sv, st, sr, svn, svl, sc, sp);
  auto reld = [](double a, double c, double floor) {
    return std::abs(a - c) / std::max({std::abs(a), std::abs(c), floor});
  };
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_LT(reld(ap[i], sp[i], 1e-6), 1e-9) << "price i=" << i;
    EXPECT_LT(reld(ad[i], sd[i], 1e-6), 1e-8) << "delta i=" << i;
    EXPECT_LT(reld(ag[i], sg[i], 1e-6), 1e-5) << "gamma i=" << i;
    EXPECT_LT(reld(av_[i], sv[i], 1e-4), 1e-7) << "vega i=" << i;
    EXPECT_LT(reld(ar[i], sr[i], 1e-4), 1e-6) << "rho i=" << i;
    EXPECT_LT(reld(avn[i], svn[i], 1e-5), 1e-5) << "vanna i=" << i;
    EXPECT_LT(reld(avl[i], svl[i], 1e-3), 1e-4) << "volga i=" << i;
    EXPECT_LT(reld(at[i], st[i], 1e-4), 1e-5) << "theta i=" << i;
    EXPECT_LT(reld(ac[i], sc[i], 1e-4), 1e-4) << "charm i=" << i;
  }
}

// K4 first-order tier: requesting only base-boundary greeks (delta/gamma/theta/price)
// skips the sigma+/-, r+/- and speed solves, but the columns it DOES return must be
// BIT-IDENTICAL to the full-bundle run (same base boundary, same stencils, same PDE).
TEST(AmericanGreeksBatchLaned, FirstOrderMaskBitMatchesFullBundle) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on host";
  }
  const Book b = make_american_greeks_book();
  const std::size_t n = b.size();
  auto run = [&](GreekFieldMask fields, std::vector<double>& dl, std::vector<double>& gm,
                 std::vector<double>& th, std::vector<double>& px) {
    dl.assign(n, -1); gm.assign(n, -1); th.assign(n, -1); px.assign(n, -1);
    simd::GreeksBatchSoA out{dl.data(), gm.data(), nullptr, th.data(),
                             nullptr, nullptr, nullptr, nullptr, px.data()};
    PricingKernel kernel;
    kernel.analytic_greeks = true;
    kernel.isa = simd::SimdIsa::ForceAvx2;
    PricingWorkspace ws;
    ASSERT_TRUE(american_greeks_batch(b.view(), fields, out, kernel, ws).has_value());
  };
  std::vector<double> fd, fg, ft, fp; // first-order request
  std::vector<double> ad, ag, at, ap; // full bundle
  run(GreekFieldMask::Delta | GreekFieldMask::Gamma | GreekFieldMask::Theta |
          GreekFieldMask::Price,
      fd, fg, ft, fp);
  run(GreekFieldMask::All, ad, ag, at, ap);
  for (std::size_t i = 0; i < n; ++i) {
    if (b.side[i] != Side::Put) {
      continue; // calls go through the scalar fan either way
    }
    EXPECT_EQ(fp[i], ap[i]) << "price i=" << i;
    EXPECT_EQ(fd[i], ad[i]) << "delta i=" << i;
    EXPECT_EQ(fg[i], ag[i]) << "gamma i=" << i;
    EXPECT_EQ(ft[i], at[i]) << "theta i=" << i;
  }
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

// ── FIX-5 / I3 — the SECOND laned-Greek driver: american_greeks_batch's two
//    Ok-stamps must guard the REQUESTED set, and must agree across sides. ─────
//
// american_greeks_batch stamps LaneStatus from two different places:
//   * the scalar route (`price_lane`) — used by every lane on the FD route and by
//     the CALL lanes on the analytic route — stamped Ok whenever the bundle merely
//     `has_value()`;
//   * the laned route (`flush`) — the analytic PUT lanes — stamped from
//     `isfinite(price)` alone.
// Neither consulted GreekNeeds, so a bundle with a finite mark and a NaN in a
// REQUESTED column was certified Ok on both; and because the two predicates differ,
// the same input could be demoted as a put and certified as a call. That is the F1
// defect class (same output column, same request, answer depending on which lane the
// kernel handled) reproduced on STATUS rather than on value.
//
// The defect is pre-existing, but WS-Y made this function public Python API
// (python/src/bindings/pricing.cpp, called with GreekFieldMask::All — every Greek
// requested and, pre-fix, none guarded), so the sprint changed its exposure.
//
// Trigger is G1's: at S = 1e-160 the FD gamma denominator hS*hS = (1e-3*S)^2
// underflows to 0.0 while the mark stays finite, so gamma = 0/0 = NaN on a
// SUCCESSFUL bundle. Both sides are driven in ONE call.
constexpr double kI3TinyS = 1.0e-160;

TEST(AmericanGreeksBatch, I3_OkStampGuardsTheRequestedGreekSetOnBothSides) {
  // Kernel-level precondition, asserted for BOTH sides: a SUCCESSFUL differenced
  // bundle with a finite mark and a non-finite gamma.
  for (const Side sd : {Side::Put, Side::Call}) {
    const auto probe =
        american_greeks_fd(kI3TinyS, 100.0, 0.05, 0.9, 0.043, 0.02, sd);
    ASSERT_TRUE(probe.has_value()) << "side " << static_cast<int>(sd);
    EXPECT_TRUE(std::isfinite(probe->price)) << "side " << static_cast<int>(sd);
    EXPECT_FALSE(std::isfinite(probe->gamma)) << "side " << static_cast<int>(sd);
  }

  // Identical inputs modulo `side`, in the SAME call — the asymmetry's own shape.
  Book b;
  b.push(kI3TinyS, 100.0, 0.05, 0.9, 0.043, 0.02, Side::Put);
  b.push(kI3TinyS, 100.0, 0.05, 0.9, 0.043, 0.02, Side::Call);
  const std::size_t n = b.size();
  std::vector<double> px(n, 0.0), gm(n, 0.0), dl(n, 0.0);
  simd::GreeksBatchSoA out;
  out.price = px.data();
  out.gamma = gm.data();
  out.delta = dl.data();
  PricingKernel kernel; // FD route (analytic_greeks defaults false)
  PricingWorkspace ws;
  // GreekFieldMask::All is exactly what the Python binding passes.
  ASSERT_TRUE(american_greeks_batch(b.view(), GreekFieldMask::All, out, kernel, ws)
                  .has_value());

  // Half 1 — a NaN in a REQUESTED column may not be certified Ok. Pre-fix both
  // lanes were LaneStatus::Ok carrying a NaN gamma.
  EXPECT_EQ(ws.lane_status_view()[0], LaneStatus::Unsupported) << "put lane";
  EXPECT_EQ(ws.lane_status_view()[1], LaneStatus::Unsupported) << "call lane";

  // Half 2 — and the two sides agree, which is the property that makes the
  // put/call split structurally impossible rather than merely absent today.
  EXPECT_EQ(ws.lane_status_view()[0], ws.lane_status_view()[1])
      << "same input modulo side, different status";
}

TEST(AmericanGreeksBatch, I3_AnalyticRoutePutAndCallStampsAgree) {
  // The analytic route is where the two stamps physically diverge: PUT lanes go
  // through the laned bundle (isfinite(price)) and CALL lanes through the scalar
  // fan (has_value()). Same input modulo side, same call, so whatever either
  // stamp decides, both must decide it — and an Ok lane must carry finite
  // requested Greeks.
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "analytic laned put route needs an AVX2 host";
  }
  Book b;
  b.push(kI3TinyS, 100.0, 0.05, 0.9, 0.043, 0.02, Side::Put);
  b.push(kI3TinyS, 100.0, 0.05, 0.9, 0.043, 0.02, Side::Call);
  const std::size_t n = b.size();
  std::vector<double> px(n, 0.0), gm(n, 0.0), dl(n, 0.0), th(n, 0.0);
  simd::GreeksBatchSoA out;
  out.price = px.data();
  out.gamma = gm.data();
  out.delta = dl.data();
  out.theta = th.data();
  PricingKernel kernel;
  kernel.analytic_greeks = true;
  kernel.isa = simd::SimdIsa::Auto;
  PricingWorkspace ws;
  ASSERT_TRUE(american_greeks_batch(b.view(), GreekFieldMask::All, out, kernel, ws)
                  .has_value());

  EXPECT_EQ(ws.lane_status_view()[0], ws.lane_status_view()[1])
      << "analytic put (laned) and call (scalar) stamps disagree on the same input";
  for (std::size_t i = 0; i < n; ++i) {
    if (ws.lane_status_view()[i] != LaneStatus::Ok) {
      continue;
    }
    // An Ok lane's REQUESTED columns are finite — the whole point of the stamp.
    EXPECT_TRUE(std::isfinite(px[i])) << "lane " << i << " price";
    EXPECT_TRUE(std::isfinite(dl[i])) << "lane " << i << " delta";
    EXPECT_TRUE(std::isfinite(gm[i])) << "lane " << i << " gamma";
    EXPECT_TRUE(std::isfinite(th[i])) << "lane " << i << " theta";
  }
}

} // namespace
} // namespace atx::vol
