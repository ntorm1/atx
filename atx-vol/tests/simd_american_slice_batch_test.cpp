#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/american_batch.hpp"
#include "atx/vol/counters.hpp"
#include "atx/vol/simd/cpu.hpp"

// Sub-Sprint A, Task A2 — batched node-solve entry proof.
//
// The slice-sigma de-Americanization path solves many independent single-boundary
// American puts (one per (strike, sigma) node). Task A2's deliverable is that these
// node solves go through the A1 AVX2 boundary batch in ceil(N/4) 4-wide packs rather
// than a scalar per-node loop, with results identical to the scalar path and a
// preserved scalar fallback. The batched entry Sprint I wires the slice pricer onto
// is `american_price_batch_resolved` (broadcast S,T,r,q + per-lane K,sigma,side — the
// exact shape of a put slice's cold-fallback stream); it groups genuine single-
// boundary lanes into complete AVX packs, dispatches each through
// simd::american_put_boundary_batch (Task A1), counts AmericanAvxPackDispatches per
// AVX2 pack, and reports the per-lane pack route. This suite proves that contract.
//
// NOTE (file ownership): the slice pricer itself (slice_sigma_impl /
// SigmaBoundaryInterp::build) lives in boundary_interp.cpp, which is another sprint's
// TU; wiring it onto this batch is deferred to Sprint I per the sub-sprint plan. This
// suite validates the entry + the ceil(N/4) batching + parity + fallback that Sprint I
// consumes.

namespace atx::vol {
namespace {

// Restore the process-global ISA override after each test (american_price_batch_resolved
// takes its ISA per-request, but other suites share the process and read the override).
struct IsaGuard {
  IsaGuard() = default;
  ~IsaGuard() { simd::set_simd_isa_override(simd::SimdIsa::Auto); }
};

// A block of genuine single-boundary American puts (fixed S,T,r>0,q; varying K,sigma),
// mirroring a put slice's cold-fallback node stream.
struct PutSlice {
  double S{100.0}, T{1.0}, r{0.05}, q{0.01};
  std::vector<double> K, sigma;
  std::vector<Side> side;
  void build(std::size_t n) {
    K.resize(n);
    sigma.resize(n);
    side.assign(n, Side::Put);
    for (std::size_t i = 0; i < n; ++i) {
      K[i] = 70.0 + 60.0 * (static_cast<double>(i % 41) + 0.5) / 41.0; // 70..130
      sigma[i] = 0.15 + 0.30 * static_cast<double>(i % 23) / 23.0;     // 0.15..0.45
    }
  }
};

// Reference: the per-contract scalar cold andersen_lake for lane i (the parity oracle).
double scalar_ref(const PutSlice& s, std::size_t i) {
  const Result<double> r =
      andersen_lake(s.S, s.K[i], s.T, s.sigma[i], s.r, s.q, Side::Put, std::nullopt);
  return r.has_value() ? *r : std::numeric_limits<double>::quiet_NaN();
}

// N divisible by 4 -> ceil(N/4) == N/4 complete packs, every lane genuine.
TEST(SliceBatchNodeSolve, ForceAvx2SolvesNNodesInCeilNOver4Packs) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on host";
  }
  IsaGuard guard;
  constexpr std::size_t kN = 128; // matches the SPRINT 128-row block-ladder -> 32 packs
  PutSlice s;
  s.build(kN);
  std::vector<double> price(kN);
  std::vector<Status> status(kN);
  std::vector<simd::SimdRoute> route(kN);
  const ResolvedAmericanPriceBatchRequest request{
      .S = s.S, .T = s.T, .r = s.r, .q = s.q,
      .K = s.K, .sigma = s.sigma, .side = s.side,
      .method = AmericanMethod::AndersenLake,
      .al_opts = std::nullopt,
      .isa = simd::SimdIsa::ForceAvx2,
      .price = price, .status = status, .pack_dispatch = route,
  };

  if constexpr (counters::counters_enabled()) {
    counters::reset();
  }
  ASSERT_TRUE(american_price_batch_resolved(request).has_value());

  // Every genuine lane went through an AVX2-dispatched pack.
  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_TRUE(status[i].has_value()) << "lane " << i;
    EXPECT_EQ(route[i], simd::SimdRoute::Avx2) << "lane " << i;
  }
  // Direct ceil(N/4) proof via the pack-dispatch counter (counters-on builds).
  if constexpr (counters::counters_enabled()) {
    const counters::Snapshot snap = counters::snapshot();
    EXPECT_EQ(snap.get(counters::Counter::AmericanAvxPackDispatches), kN / 4u); // 128/4 = 32
  }
  // Results identical to the scalar per-contract solve (immateriality gate; the AVX2
  // kernel's only gap vs scalar is its vector transcendentals).
  double max_abs = 0.0;
  for (std::size_t i = 0; i < kN; ++i) {
    ASSERT_TRUE(std::isfinite(price[i])) << "lane " << i;
    max_abs = std::max(max_abs, std::abs(price[i] - scalar_ref(s, i)));
  }
  EXPECT_LE(max_abs, 1.0e-6) << "AVX2 batch vs scalar cold max|delta|";
}

// Scalar fallback preserved: ForceScalar routes every lane scalar and reproduces the
// per-contract andersen_lake bit-for-bit.
TEST(SliceBatchNodeSolve, ForceScalarFallbackIsBitIdenticalToScalarCold) {
  IsaGuard guard;
  constexpr std::size_t kN = 64;
  PutSlice s;
  s.build(kN);
  std::vector<double> price(kN);
  std::vector<Status> status(kN);
  std::vector<simd::SimdRoute> route(kN);
  const ResolvedAmericanPriceBatchRequest request{
      .S = s.S, .T = s.T, .r = s.r, .q = s.q,
      .K = s.K, .sigma = s.sigma, .side = s.side,
      .method = AmericanMethod::AndersenLake,
      .al_opts = std::nullopt,
      .isa = simd::SimdIsa::ForceScalar,
      .price = price, .status = status, .pack_dispatch = route,
  };
  ASSERT_TRUE(american_price_batch_resolved(request).has_value());
  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(route[i], simd::SimdRoute::Scalar) << "lane " << i;
    EXPECT_EQ(price[i], scalar_ref(s, i)) << "lane " << i; // bit-identical
  }
}

// N not divisible by 4: floor(N/4) complete AVX2 packs, the < 4 tail patched scalar —
// documents that the batched entry never leaves a partial pack unsolved.
TEST(SliceBatchNodeSolve, NonMultipleOfFourFlushesTailScalar) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on host";
  }
  IsaGuard guard;
  constexpr std::size_t kN = 130; // 32 full packs + 2 tail lanes
  PutSlice s;
  s.build(kN);
  std::vector<double> price(kN);
  std::vector<Status> status(kN);
  std::vector<simd::SimdRoute> route(kN);
  const ResolvedAmericanPriceBatchRequest request{
      .S = s.S, .T = s.T, .r = s.r, .q = s.q,
      .K = s.K, .sigma = s.sigma, .side = s.side,
      .method = AmericanMethod::AndersenLake,
      .al_opts = std::nullopt,
      .isa = simd::SimdIsa::ForceAvx2,
      .price = price, .status = status, .pack_dispatch = route,
  };
  if constexpr (counters::counters_enabled()) {
    counters::reset();
  }
  ASSERT_TRUE(american_price_batch_resolved(request).has_value());
  if constexpr (counters::counters_enabled()) {
    const counters::Snapshot snap = counters::snapshot();
    EXPECT_EQ(snap.get(counters::Counter::AmericanAvxPackDispatches), kN / 4u); // floor(130/4)=32
  }
  std::size_t n_scalar = 0;
  for (std::size_t i = 0; i < kN; ++i) {
    ASSERT_TRUE(std::isfinite(price[i]));
    if (route[i] == simd::SimdRoute::Scalar) {
      ++n_scalar;
    }
    EXPECT_LE(std::abs(price[i] - scalar_ref(s, i)), 1.0e-6) << "lane " << i;
  }
  EXPECT_EQ(n_scalar, kN % 4u); // exactly the 2-lane tail
}

} // namespace
} // namespace atx::vol
