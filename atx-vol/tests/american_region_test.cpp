#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_region.hpp" // the classification under test
#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/simd/american_boundary_batch.hpp" // the laned batch route (no-price test)
#include "atx/vol/api/simd/cpu.hpp"                     // use_avx2 (honours ATX_SIMD_ISA)
#include "pricing/american_boundary.hpp"                // amer:: seam: the two scalar sites

#if defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>

#include "simd/american_boundary_avx2_kernel.hpp" // the laned price site
#endif

// C1 exercise-region semantics (2026-10-22). One classification
// (classify_exercise_region) for every Andersen-Lake price site and route; these tests pin
// that the four sites — scalar uncached, scalar cached, laned AVX2 and the warm-started ALO
// pricer (AloRegionPricer, C1 fix round 1) — agree on it bit for bit (near-boundary and
// degenerate inputs included), that the classified entry is the rule entry plus its class,
// the genuine exercise plateau (exact intrinsic, zero vega), the plateau edge's continuity,
// the price limits, the carry regimes of both sides, and that the double-continuation
// regime never yields a price on any route.
//
// Error classes used below: the L1c probe's noise floor 1e-12 * max(1, P) for a sigma step
// (american_monotone_test.cpp; LEDGER 2026-10-19), and the accurate preset's in-region
// error class 5.1e-7 * K * max(1, T)^2 (AlMonotone grid, reference-backed) for deep ITM
// cells, and its edge class kAccurateEdgeClass (below, reference-backed) for the edge.

namespace {

using atx::core::ErrorCode;
using atx::core::Result;
using atx::vol::AlExerciseClass;
using atx::vol::AlExerciseRegion;
using atx::vol::AlExpiryGeometry;
using atx::vol::AlOpts;
using atx::vol::AloPricer;
using atx::vol::AloRegionPricer;
using atx::vol::AlRegionPrice;
using atx::vol::andersen_lake;
using atx::vol::andersen_lake_classified;
using atx::vol::classify_exercise_region;
using atx::vol::kAlExpiryLimitT;
using atx::vol::kAlZeroVolSigma;
using atx::vol::Side;
namespace amer = atx::vol::amer;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNoiseFloor = 1.0e-12;    // relative sigma-step tolerance (L1c)
constexpr double kAccurateClass = 5.1e-7;  // accurate in-region error / (K max(1, T)^2)

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] double intrinsic_of(double S, double K, Side side) noexcept {
  return (side == Side::Put) ? (K - S) : (S - K);
}

[[nodiscard]] std::size_t class_index(AlExerciseClass c) noexcept {
  return static_cast<std::size_t>(c);
}

// Andersen-Lake under an explicit rule and its class, accurate preset unless given.
[[nodiscard]] Result<AlRegionPrice> classified(double S, double K, double T, double sigma,
                                               double r, double q, Side side,
                                               AlExerciseRegion rule,
                                               const std::optional<AlOpts> &opts = std::nullopt) {
  return andersen_lake_classified(S, K, T, sigma, r, q, side, opts, rule);
}

// ── 1. The classifier ─────────────────────────────────────────────────────────

TEST(AmericanExerciseRegion, ClassifierFollowsTheDispatchOrderAndTheCallMirror) {
  struct Case {
    double S, K, T, r, q, sigma;
    bool call;
    double b;
    AlExerciseClass want;
  };
  const double b = 90.0;
  const double t_live = std::nextafter(kAlExpiryLimitT, kInf);
  const double s_live = std::nextafter(kAlZeroVolSigma, kInf);
  const std::array<Case, 18> cases{{
      // Degenerate T first: the payoff is exact in every regime, the unsupported one too.
      {100.0, 120.0, 0.0, 0.05, 0.02, 0.2, false, b, AlExerciseClass::ExpiryLimit},
      {100.0, 120.0, kAlExpiryLimitT, -0.01, -0.03, 0.2, false, b, AlExerciseClass::ExpiryLimit},
      // Then degenerate sigma, before the regime (the dispatch prices it in every regime).
      {100.0, 120.0, t_live, 0.05, 0.02, 0.0, false, b, AlExerciseClass::ZeroVolLimit},
      {100.0, 120.0, 1.0, -0.01, -0.03, kAlZeroVolSigma, false, b,
       AlExerciseClass::ZeroVolLimit},
      // The carry regime of the internal put (a call's is rate q, yield r).
      {100.0, 120.0, 1.0, 0.0, 0.01, 0.2, false, b, AlExerciseClass::European},
      {100.0, 120.0, 1.0, -0.01, 0.0, 0.2, false, b, AlExerciseClass::European},
      {100.0, 120.0, 1.0, -0.01, -0.01, 0.2, false, b, AlExerciseClass::European},
      {100.0, 120.0, 1.0, -0.01, -0.03, 0.2, false, b, AlExerciseClass::UnsupportedRegime},
      {100.0, 80.0, 1.0, 0.01, 0.0, 0.2, true, b, AlExerciseClass::European},
      {100.0, 80.0, 1.0, -0.03, -0.01, 0.2, true, b, AlExerciseClass::UnsupportedRegime},
      // The American arm: the internal put's spot (put S, call K) against the boundary.
      {90.0, 120.0, 1.0, 0.05, 0.02, s_live, false, b, AlExerciseClass::ExercisePlateau},
      {std::nextafter(90.0, 0.0), 120.0, 1.0, 0.05, 0.02, 0.2, false, b,
       AlExerciseClass::ExercisePlateau},
      {std::nextafter(90.0, kInf), 120.0, 1.0, 0.05, 0.02, 0.2, false, b,
       AlExerciseClass::Continuation},
      {100.0, 120.0, 1.0, 0.05, 0.02, 0.2, false, kNaN, AlExerciseClass::Continuation},
      {kNaN, 120.0, 1.0, 0.05, 0.02, 0.2, false, b, AlExerciseClass::Continuation},
      {100.0, 120.0, 1.0, 0.0, -0.01, 0.2, false, 100.0, AlExerciseClass::ExercisePlateau},
      {130.0, 90.0, 1.0, 0.01, 0.06, 0.2, true, b, AlExerciseClass::ExercisePlateau},
      {130.0, std::nextafter(90.0, kInf), 1.0, 0.01, 0.06, 0.2, true, b,
       AlExerciseClass::Continuation},
  }};
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const Case &c = cases[i];
    EXPECT_EQ(classify_exercise_region(c.S, c.K, c.T, c.r, c.q, c.sigma, c.call, c.b), c.want)
        << "case " << i;
  }
  // A call is its McDonald-Schroder internal put, for every class.
  int mirrored = 0;
  for (const double S : {50.0, 100.0, 150.0}) {
    for (const double K : {50.0, 100.0, 150.0}) {
      for (const double T : {0.0, 0.5}) {
        for (const auto &rq : std::array<std::array<double, 2>, 6>{
                 {{-0.03, -0.01}, {0.05, 0.02}, {0.02, 0.06}, {0.0, -0.01}, {-0.01, 0.0},
                  {-0.01, -0.03}}}) {
          for (const double sg : {0.0, 0.2}) {
            for (const double bb : {kNaN, 60.0, 100.0, 140.0}) {
              EXPECT_EQ(classify_exercise_region(S, K, T, rq[0], rq[1], sg, true, bb),
                        classify_exercise_region(K, S, T, rq[1], rq[0], sg, false, bb));
              ++mirrored;
            }
          }
        }
      }
    }
  }
  EXPECT_EQ(mirrored, 864);
  EXPECT_EQ(std::string_view(to_string(AlExerciseClass::Continuation)), "continuation");
  EXPECT_EQ(std::string_view(to_string(AlExerciseClass::ExercisePlateau)), "exercise_plateau");
  EXPECT_EQ(std::string_view(to_string(AlExerciseClass::European)), "european");
  EXPECT_EQ(std::string_view(to_string(AlExerciseClass::ExpiryLimit)), "expiry_limit");
  EXPECT_EQ(std::string_view(to_string(AlExerciseClass::ZeroVolLimit)), "zero_vol_limit");
  EXPECT_EQ(std::string_view(to_string(AlExerciseClass::UnsupportedRegime)),
            "unsupported_regime");
}

// ── 2. The classified entry (the hook K4 wires into the family / inversion routes) ──

struct Contract {
  double S, K, T, sigma, r, q;
  Side side;
};

[[nodiscard]] std::vector<Contract> route_grid() {
  std::vector<Contract> out;
  constexpr double S = 100.0;
  for (const Side side : {Side::Put, Side::Call}) {
    for (const double T : {0.0, 1.0e-13, 1.0e-6, 0.25, 2.0}) {
      for (const double sg : {0.0, 1.0e-9, 0.05, 0.3}) {
        for (const auto &rq : std::array<std::array<double, 2>, 7>{
                 {{0.05, 0.02}, {0.02, 0.08}, {0.0, -0.02}, {0.0, 0.01}, {-0.01, 0.0},
                  {-0.01, -0.03}, {-0.03, -0.01}}}) {
          for (const double m : {0.4, 0.9, 1.0, 1.25, 2.5}) {
            const double K = (side == Side::Put) ? S * m : S / m;
            out.push_back(Contract{S, K, T, sg, rq[0], rq[1], side});
          }
        }
      }
    }
  }
  return out;
}

TEST(AmericanExerciseRegion, ClassifiedEntryIsBitwiseTheRuleEntryAndCarriesItsClass) {
  const std::array<std::optional<AlOpts>, 2> presets{std::nullopt, atx::vol::al_fast_opts()};
  std::array<int, 6> seen{};
  for (const std::optional<AlOpts> &o : presets) {
    for (const AlExerciseRegion rule :
         {AlExerciseRegion::KernelFloor, AlExerciseRegion::Intrinsic}) {
      for (const Contract &c : route_grid()) {
        const Result<double> legacy =
            andersen_lake(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, o, rule);
        const Result<AlRegionPrice> cl =
            andersen_lake_classified(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, o, rule);
        if (!cl) {
          ASSERT_FALSE(legacy.has_value());
          EXPECT_EQ(cl.error().code(), legacy.error().code());
          continue;
        }
        ++seen[class_index(cl->cls)];
        const bool is_call = (c.side == Side::Call);
        EXPECT_EQ(cl->cls, classify_exercise_region(c.S, c.K, c.T, c.r, c.q, c.sigma, is_call,
                                                    cl->boundary));
        const bool armed = cl->cls == AlExerciseClass::Continuation ||
                           cl->cls == AlExerciseClass::ExercisePlateau;
        EXPECT_EQ(std::isfinite(cl->boundary), armed) << to_string(cl->cls);
        if (cl->priced()) {
          ASSERT_TRUE(legacy.has_value()) << to_string(cl->cls);
          EXPECT_TRUE(same_bits(cl->price, *legacy)) << to_string(cl->cls) << " K " << c.K;
        } else {
          ASSERT_FALSE(legacy.has_value());
          EXPECT_EQ(legacy.error().code(), ErrorCode::NotImplemented);
          EXPECT_TRUE(std::isnan(cl->price) && std::isnan(cl->boundary));
        }
        if (rule == AlExerciseRegion::Intrinsic && cl->cls == AlExerciseClass::ExercisePlateau) {
          EXPECT_TRUE(same_bits(cl->price, intrinsic_of(c.S, c.K, c.side)));
        }
        // The prepared-geometry overload: the same price, class and boundary bits.
        const AlExpiryGeometry g = atx::vol::prepare_al_geometry(c.T, c.r, c.q, o);
        const Result<AlRegionPrice> gc =
            andersen_lake_classified(g, c.S, c.K, c.sigma, c.side, rule);
        const Result<double> gl = andersen_lake(g, c.S, c.K, c.sigma, c.side, rule);
        if (!gc) {
          ASSERT_FALSE(gl.has_value());
          EXPECT_EQ(gc.error().code(), gl.error().code());
          continue;
        }
        EXPECT_EQ(gc->cls, cl->cls) << "geometry, K " << c.K << " T " << c.T;
        EXPECT_TRUE(same_bits(gc->price, cl->price) ||
                    (std::isnan(gc->price) && std::isnan(cl->price)))
            << "geometry, K " << c.K << " T " << c.T;
        EXPECT_TRUE(same_bits(gc->boundary, cl->boundary) ||
                    (std::isnan(gc->boundary) && std::isnan(cl->boundary)))
            << "geometry boundary, K " << c.K << " T " << c.T;
        if (gc->priced()) {
          ASSERT_TRUE(gl.has_value());
          EXPECT_TRUE(same_bits(gc->price, *gl));
        }
      }
    }
  }
  for (std::size_t i = 0; i < seen.size(); ++i) {
    EXPECT_GT(seen[i], 0) << "coverage: class " << to_string(static_cast<AlExerciseClass>(i));
  }
}

// ── 3. The four price sites agree on the classification, bit for bit ───────────────

// The put-coordinate contract a site sees (a call arrives as its internal put).
struct SiteInput {
  double K, T, sigma, r, q;
};

[[nodiscard]] std::vector<SiteInput> site_contracts() {
  std::vector<SiteInput> out;
  for (const double K : {80.0, 100.0, 125.0}) {
    for (const double T : {1.0e-13, 0.05, 0.5, 2.0, 5.0}) {
      for (const double sg : {1.0e-9, 0.03, 0.2, 0.6, 1.5}) {
        for (const auto &rq : std::array<std::array<double, 2>, 8>{
                 {{0.05, 0.02}, {0.02, 0.06}, {0.03, 0.0}, {0.0, -0.02}, {0.08, 0.08},
                  {0.0, 0.01}, {-0.01, 0.0}, {-0.01, -0.03}}}) {
          out.push_back(SiteInput{K, T, sg, rq[0], rq[1]});
        }
      }
    }
  }
  return out;
}

// 20 spots per lane: the boundary B and its two neighbouring doubles, relative bumps of
// 1e-12 / 1e-9 / 1e-6 either side, and a moneyness ladder (deep ITM to deep OTM).
constexpr std::size_t kSpots = 20;
[[nodiscard]] std::array<double, kSpots> spot_ladder(double B, double K) {
  const double base = std::isfinite(B) ? B : 0.9 * K;
  return {{base, std::nextafter(base, 0.0), std::nextafter(base, kInf), base * (1.0 - 1.0e-12),
           base * (1.0 + 1.0e-12), base * (1.0 - 1.0e-9), base * (1.0 + 1.0e-9),
           base * (1.0 - 1.0e-6), base * (1.0 + 1.0e-6), 0.05 * K, 0.3 * K, 0.6 * K, 0.9 * K,
           0.99 * K, K, 1.01 * K, 1.2 * K, 1.5 * K, 2.0 * K, 4.0 * K}};
}

[[nodiscard]] bool american_live(const SiteInput &c) noexcept {
  const AlExerciseClass pre =
      classify_exercise_region(c.K, c.K, c.T, c.r, c.q, c.sigma, false, kNaN);
  return pre == AlExerciseClass::Continuation; // American arm, no boundary given
}

[[nodiscard]] bool same_or_nan(double a, double b) noexcept {
  return same_bits(a, b) || (std::isnan(a) && std::isnan(b));
}

// The four sites: the scalar uncached site, the scalar cached site (cache hit and inline),
// the laned AVX2 kernel, and the warm-started ALO pricer (AloRegionPricer, the scalar
// inversion's residual map; C1 fix round 1). Every site is handed ONE boundary per lane —
// the ALO's own cold solve, copied into the scalar boundary and the pack's Y lane — so a
// disagreement can only come from the decision itself. The ALO is reset to each point and
// re-solves cold from the same inputs (the boundary does not depend on the spot); the test
// pins that it re-converges the same B(T) bits it classified against.
TEST(AmericanExerciseRegion, FourSitesAgreeOnTheClassificationBitForBit) {
  const amer::AlScheme sch = amer::scheme_from_opts(std::nullopt);
  // The laned site runs where the process may take the vector path (ATX_SIMD_ISA honoured,
  // so the ForceScalar lane checks the three scalar sites against each other only).
  bool laned = false;
#if defined(__AVX2__) && defined(__FMA__)
  laned = atx::vol::simd::use_avx2();
#endif
  // Lane 0 of every pack is a fixed American contract, so each pack has an eligible lane
  // and its geometry is filled; lanes 1-3 walk the grid.
  const SiteInput anchor{100.0, 1.0, 0.25, 0.05, 0.02};
  const std::vector<SiteInput> grid = site_contracts();
  AloRegionPricer alo(anchor.K, anchor.K, anchor.T, anchor.r, anchor.q, Side::Put);
  std::size_t points = 0;
  std::size_t alo_points = 0;
  std::size_t near_edge = 0;
  std::size_t plateau = 0;
  std::size_t unsolved = 0;
  std::size_t solve_mismatch = 0;
  std::array<std::size_t, 6> seen{};
  for (std::size_t g0 = 0; g0 < grid.size(); g0 += 3) {
    std::array<SiteInput, 4> pack_in{anchor, anchor, anchor, anchor};
    for (std::size_t l = 1; l < 4 && g0 + l - 1 < grid.size(); ++l) {
      pack_in[l] = grid[g0 + l - 1];
    }
    std::array<amer::AlBoundary, 4> bnd{};
    std::array<amer::AlWorkspace, 4> ws{};
    std::array<bool, 4> solved{};
    std::array<bool, 4> alo_solved{};
#if defined(__AVX2__) && defined(__FMA__)
    atx::vol::simd::detail::PutPackBoundary pack;
    if (laned) {
      alignas(32) std::array<double, 4> K{}, T{}, sg{}, r{}, q{};
      for (std::size_t l = 0; l < 4; ++l) {
        K[l] = pack_in[l].K;
        T[l] = pack_in[l].T;
        sg[l] = pack_in[l].sigma;
        r[l] = pack_in[l].r;
        q[l] = pack_in[l].q;
      }
      bool eligible[4] = {false, false, false, false};
      int ref = -1;
      // The solve needs a positive spot per lane only for its eligibility screen.
      atx::vol::simd::detail::solve_put_boundary_pack_avx2(K.data(), K.data(), T.data(),
                                                           sg.data(), r.data(), q.data(), 4, sch,
                                                           bnd.data(), ws.data(), pack,
                                                           eligible, ref);
      ASSERT_GE(ref, 0) << "the anchor lane is always eligible";
      for (std::size_t l = 0; l < 4; ++l) {
        solved[l] = eligible[l];
        if (!solved[l]) {
          continue;
        }
        // Hand the scalar sites the laned boundary (replaced by the ALO's below).
        for (unsigned j = 0; j < pack.nb; ++j) {
          alignas(32) double yl[4];
          _mm256_store_pd(yl, pack.Y[j]);
          bnd[l].y[j] = yl[l];
        }
      }
    }
#endif
    if (!laned) {
      for (std::size_t l = 0; l < 4; ++l) {
        const SiteInput &c = pack_in[l];
        solved[l] = american_live(c) &&
                    amer::al_solve_put_boundary(c.K, c.T, c.sigma, c.r, c.q, sch, bnd[l], ws[l]) ==
                        amer::AlSolveStatus::Ok;
      }
    }
    // The ALO's cold solve is the boundary every site is handed: the same Y bits in all four.
    for (std::size_t l = 0; l < 4; ++l) {
      const SiteInput &c = pack_in[l];
      if (!american_live(c)) {
        continue; // decided before any boundary: no solve on any site
      }
      alo.reset(c.K, c.K, c.T, c.r, c.q, Side::Put);
      alo_solved[l] = !std::isnan(alo.price(c.sigma, AlExerciseRegion::KernelFloor));
      if (alo_solved[l] != solved[l]) {
        ++solve_mismatch; // one solver failed where the other converged: no shared boundary
        continue;
      }
      if (!solved[l]) {
        continue;
      }
      const amer::AlBoundary &ab = amer::al_alo_boundary(alo.core());
      ASSERT_EQ(ab.n, bnd[l].n) << "one scheme, one node count";
      ASSERT_TRUE(same_bits(ab.xmax, bnd[l].xmax)) << "one asymptotic level";
      for (unsigned j = 0; j < ab.n; ++j) {
        bnd[l].y[j] = ab.y[j];
      }
#if defined(__AVX2__) && defined(__FMA__)
      if (laned) {
        ASSERT_EQ(pack.nb, ab.n);
        alignas(32) double xm[4];
        _mm256_store_pd(xm, pack.XMAX);
        ASSERT_TRUE(same_bits(xm[l], ab.xmax)) << "lane " << l;
        for (unsigned j = 0; j < ab.n; ++j) {
          alignas(32) double yl[4];
          _mm256_store_pd(yl, pack.Y[j]);
          yl[l] = ab.y[j];
          pack.Y[j] = _mm256_load_pd(yl);
        }
      }
#endif
    }
    std::array<double, 4> B{};
    std::array<std::array<double, kSpots>, 4> spots{};
    for (std::size_t l = 0; l < 4; ++l) {
      B[l] = solved[l] ? amer::al_boundary_at_expiry(bnd[l]) : kNaN;
      if (solved[l]) {
        // The value the L3k region test read is the same bits.
        EXPECT_TRUE(same_bits(B[l], amer::al_boundary_now(bnd[l])));
        EXPECT_TRUE(same_bits(B[l], amer::al_boundary_at_expiry(bnd[l].y[bnd[l].n - 1u],
                                                                bnd[l].xmax)));
      }
      spots[l] = spot_ladder(B[l], pack_in[l].K);
    }
    for (std::size_t i = 0; i < kSpots; ++i) {
      std::array<AlExerciseClass, 4> cls_laned{};
      std::array<AlExerciseClass, 4> cls_laned_floor{};
      alignas(32) std::array<double, 4> px_laned{};
      alignas(32) std::array<double, 4> px_laned_floor{};
      alignas(32) std::array<double, 4> px_laned_default{};
#if defined(__AVX2__) && defined(__FMA__)
      if (laned) {
        alignas(32) std::array<double, 4> sp{};
        for (std::size_t l = 0; l < 4; ++l) {
          sp[l] = spots[l][i];
        }
        const __m256d spv = _mm256_load_pd(sp.data());
        _mm256_store_pd(px_laned.data(), atx::vol::simd::detail::price_put_pack_at_avx2(
                                             pack, spv, pack.Kv, pack.XMAX,
                                             AlExerciseRegion::Intrinsic, cls_laned.data()));
        // Review m2: KernelFloor with a class output prices like the scalar sites.
        _mm256_store_pd(px_laned_floor.data(),
                        atx::vol::simd::detail::price_put_pack_at_avx2(
                            pack, spv, pack.Kv, pack.XMAX, AlExerciseRegion::KernelFloor,
                            cls_laned_floor.data()));
        _mm256_store_pd(px_laned_default.data(), atx::vol::simd::detail::price_put_pack_at_avx2(
                                                     pack, spv, pack.Kv, pack.XMAX));
      }
#endif
      for (std::size_t l = 0; l < 4; ++l) {
        const SiteInput &c = pack_in[l];
        const double S = spots[l][i];
        const AlExerciseClass want = classify_exercise_region(S, c.K, c.T, c.r, c.q, c.sigma,
                                                              false, B[l]);
        ++points;
        ++seen[class_index(want)];
        if (laned) {
          EXPECT_EQ(cls_laned[l], want) << "laned, K " << c.K << " T " << c.T << " spot " << i;
          EXPECT_EQ(cls_laned_floor[l], want) << "laned KernelFloor, K " << c.K << " T " << c.T;
        }
        // The fourth site, wherever it shares the boundary (or no site has one).
        if (alo_solved[l] == solved[l]) {
          ++alo_points;
          AlExerciseClass ca = AlExerciseClass::UnsupportedRegime;
          AlExerciseClass cf = AlExerciseClass::UnsupportedRegime;
          alo.reset(S, c.K, c.T, c.r, c.q, Side::Put);
          const double pa = alo.price(c.sigma, AlExerciseRegion::Intrinsic, &ca);
          if (solved[l]) {
            EXPECT_TRUE(same_bits(
                amer::al_boundary_at_expiry(amer::al_alo_boundary(alo.core())), B[l]))
                << "the ALO re-converges the shared B(T), K " << c.K << " T " << c.T;
          }
          alo.reset(S, c.K, c.T, c.r, c.q, Side::Put);
          const double pfa = alo.price(c.sigma, AlExerciseRegion::KernelFloor, &cf);
          EXPECT_EQ(ca, want) << "ALO, K " << c.K << " T " << c.T << " spot " << i;
          EXPECT_EQ(cf, want) << "ALO KernelFloor, K " << c.K << " T " << c.T << " spot " << i;
          const double intr = c.K - S;
          switch (want) {
          case AlExerciseClass::ExercisePlateau:
            EXPECT_TRUE(same_bits(pa, intr)) << "ALO plateau price";
            EXPECT_GE(pfa, intr) << "ALO KernelFloor never prices under intrinsic";
            break;
          case AlExerciseClass::Continuation:
            EXPECT_TRUE(same_or_nan(pa, pfa)) << "ALO continuation: the rules agree";
            EXPECT_EQ(std::isnan(pa), !solved[l]) << "ALO: NaN exactly when nothing solved";
            break;
          case AlExerciseClass::ExpiryLimit:
            EXPECT_TRUE(same_bits(pa, (intr > 0.0) ? intr : 0.0)) << "ALO expiry: the payoff";
            EXPECT_TRUE(same_bits(pfa, pa));
            break;
          case AlExerciseClass::ZeroVolLimit:
          case AlExerciseClass::European:
            EXPECT_TRUE(std::isfinite(pa) && same_bits(pfa, pa)) << "ALO " << to_string(want);
            break;
          case AlExerciseClass::UnsupportedRegime:
            EXPECT_TRUE(std::isnan(pa) && std::isnan(pfa)) << "ALO: no price, typed class";
            break;
          }
        }
        if (!solved[l] && want == AlExerciseClass::Continuation) {
          ++unsolved; // American contract whose solve failed: the scalar patch's business
          continue;
        }
        AlExerciseClass cu = AlExerciseClass::UnsupportedRegime;
        AlExerciseClass cu_floor = AlExerciseClass::UnsupportedRegime;
        AlExerciseClass cc_hit = AlExerciseClass::UnsupportedRegime;
        AlExerciseClass cc_inline = AlExerciseClass::UnsupportedRegime;
        const double pu = amer::al_put_price_from_boundary(
            bnd[l], ws[l], S, c.K, c.T, c.sigma, c.r, c.q, AlExerciseRegion::Intrinsic, &cu);
        const double pf =
            amer::al_put_price_from_boundary(bnd[l], ws[l], S, c.K, c.T, c.sigma, c.r, c.q,
                                             AlExerciseRegion::KernelFloor, &cu_floor);
        // An unsolved lane has no boundary to bind a premium cache to: both arms inline.
        const double pc_hit = amer::al_put_price_from_boundary_cached_probe(
            bnd[l], ws[l], S, c.K, c.T, c.sigma, c.r, c.q, AlExerciseRegion::Intrinsic,
            /*bind_cache=*/solved[l], &cc_hit);
        const double pc_inline = amer::al_put_price_from_boundary_cached_probe(
            bnd[l], ws[l], S, c.K, c.T, c.sigma, c.r, c.q, AlExerciseRegion::Intrinsic,
            /*bind_cache=*/false, &cc_inline);
        EXPECT_EQ(cu, want) << "uncached, K " << c.K << " T " << c.T << " spot " << i;
        EXPECT_EQ(cu_floor, want) << "the class does not depend on the rule";
        EXPECT_EQ(cc_hit, want) << "cached (hit), K " << c.K << " T " << c.T;
        EXPECT_EQ(cc_inline, want) << "cached (inline), K " << c.K << " T " << c.T;
        switch (want) {
        case AlExerciseClass::ExercisePlateau: {
          ++plateau;
          const double intr = c.K - S;
          EXPECT_TRUE(same_bits(pu, intr));
          EXPECT_TRUE(same_bits(pc_hit, intr));
          EXPECT_TRUE(same_bits(pc_inline, intr));
          if (laned) {
            EXPECT_TRUE(same_bits(px_laned[l], intr)) << "laned plateau price, lane " << l;
            EXPECT_TRUE(same_bits(px_laned_floor[l], px_laned_default[l]))
                << "laned KernelFloor with a class is the default floor, lane " << l;
          }
          EXPECT_GE(pf, intr) << "KernelFloor never prices under intrinsic";
          break;
        }
        case AlExerciseClass::Continuation:
          EXPECT_TRUE(same_bits(pu, pf)) << "continuation: the rules agree";
          EXPECT_TRUE(same_bits(pc_hit, pu)) << "cached == uncached (F5 contract)";
          EXPECT_TRUE(same_bits(pc_inline, pu));
          if (laned) {
            EXPECT_TRUE(same_bits(px_laned_floor[l], px_laned[l])) << "laned: the rules agree";
            EXPECT_TRUE(same_bits(px_laned_floor[l], px_laned_default[l]));
          }
          break;
        case AlExerciseClass::European:
        case AlExerciseClass::ExpiryLimit:
        case AlExerciseClass::ZeroVolLimit:
        case AlExerciseClass::UnsupportedRegime:
          // Outside the sites' precondition: every site returns its class and no price.
          EXPECT_TRUE(std::isnan(pu) && std::isnan(pf) && std::isnan(pc_hit) &&
                      std::isnan(pc_inline))
              << to_string(want);
          if (laned) {
            EXPECT_TRUE(std::isnan(px_laned[l])) << "laned, " << to_string(want);
            EXPECT_TRUE(std::isnan(px_laned_floor[l]))
                << "laned KernelFloor (review m2), " << to_string(want);
          }
          break;
        }
        if (i <= 2 && solved[l]) {
          ++near_edge;
        }
      }
    }
  }
  std::printf("[AmericanExerciseRegion] four-site grid: %zu points (%zu on the ALO site, %zu "
              "at B and +-1 ulp, %zu plateau, %zu unsolved American, %zu solve mismatches) "
              "laned=%d classes cont=%zu plat=%zu euro=%zu expiry=%zu zerovol=%zu "
              "unsupported=%zu\n",
              points, alo_points, near_edge, plateau, unsolved, solve_mismatch, laned ? 1 : 0,
              seen[0], seen[1], seen[2], seen[3], seen[4], seen[5]);
  EXPECT_GE(points, std::size_t{10000});
  EXPECT_EQ(solve_mismatch, std::size_t{0}) << "every site solves where the ALO solves";
  EXPECT_EQ(alo_points, points);
  EXPECT_GE(near_edge, std::size_t{300});
  EXPECT_GT(plateau, std::size_t{0});
  for (std::size_t c = 0; c < seen.size(); ++c) {
    EXPECT_GT(seen[c], std::size_t{0}) << "coverage: class " << c;
  }
}

// AloPricer::price(sigma) and AloRegionPricer::price(sigma, KernelFloor) are one
// implementation over one state type (C1 fix round 1): the same bits over warm sigma
// sweeps (both sides of the 12% re-seed guard), resets, both sides, every class and both
// profiles. A class output changes no bits under KernelFloor; Intrinsic changes them only
// on the plateau, to the exact intrinsic; the class is THE classifier's against the
// boundary the call converged.
TEST(AmericanExerciseRegion, AloRegionPricerKernelFloorIsAloPricerBitForBit) {
  constexpr std::array<double, 12> sweep{0.3,    0.31, 0.305,  0.25, 0.6, 0.59,
                                         0.05,   0.0,  1.0e-9, 0.2,  0.2000001, 1.5};
  const std::array<std::optional<AlOpts>, 2> presets{std::nullopt, atx::vol::al_fast_opts()};
  std::array<std::size_t, 6> seen{};
  std::size_t calls = 0;
  for (const std::optional<AlOpts> &o : presets) {
    std::optional<AloPricer> base;
    std::optional<AloRegionPricer> floor_plain;
    std::optional<AloRegionPricer> floor_cls;
    std::optional<AloRegionPricer> intr_cls;
    for (const Contract &c : route_grid()) {
      if (c.sigma != 0.3) {
        continue; // route_grid's contracts once each; the sweep supplies sigma
      }
      if (!base) {
        base.emplace(c.S, c.K, c.T, c.r, c.q, c.side, o);
        floor_plain.emplace(c.S, c.K, c.T, c.r, c.q, c.side, o);
        floor_cls.emplace(c.S, c.K, c.T, c.r, c.q, c.side, o);
        intr_cls.emplace(c.S, c.K, c.T, c.r, c.q, c.side, o);
      } else {
        base->reset(c.S, c.K, c.T, c.r, c.q, c.side, o);
        floor_plain->reset(c.S, c.K, c.T, c.r, c.q, c.side, o);
        floor_cls->reset(c.S, c.K, c.T, c.r, c.q, c.side, o);
        intr_cls->reset(c.S, c.K, c.T, c.r, c.q, c.side, o);
      }
      const bool is_call = (c.side == Side::Call);
      for (const double sg : sweep) {
        AlExerciseClass cf = AlExerciseClass::UnsupportedRegime;
        AlExerciseClass ci = AlExerciseClass::UnsupportedRegime;
        const double p0 = base->price(sg);
        const double p1 = floor_plain->price(sg, AlExerciseRegion::KernelFloor);
        const double p2 = floor_cls->price(sg, AlExerciseRegion::KernelFloor, &cf);
        const double p3 = intr_cls->price(sg, AlExerciseRegion::Intrinsic, &ci);
        ++calls;
        EXPECT_TRUE(same_or_nan(p1, p0)) << "K " << c.K << " T " << c.T << " sigma " << sg;
        EXPECT_TRUE(same_or_nan(p2, p0)) << "a class output moves no bits, K " << c.K;
        EXPECT_EQ(cf, ci) << "the class does not depend on the rule";
        const bool arm = !std::isnan(p0) && (ci == AlExerciseClass::Continuation ||
                                             ci == AlExerciseClass::ExercisePlateau);
        const double b =
            arm ? amer::al_boundary_at_expiry(amer::al_alo_boundary(intr_cls->core())) : kNaN;
        EXPECT_EQ(ci, classify_exercise_region(c.S, c.K, c.T, c.r, c.q, sg, is_call, b))
            << "K " << c.K << " T " << c.T << " sigma " << sg;
        ++seen[class_index(ci)];
        if (ci == AlExerciseClass::ExercisePlateau) {
          const double intr = intrinsic_of(c.S, c.K, c.side);
          EXPECT_TRUE(same_bits(p3, intr)) << "plateau: the exact intrinsic";
          EXPECT_GE(p0, p3);
        } else {
          EXPECT_TRUE(same_or_nan(p3, p0)) << to_string(ci) << " K " << c.K << " T " << c.T;
        }
        EXPECT_EQ(std::isnan(p0) && ci != AlExerciseClass::Continuation,
                  ci == AlExerciseClass::UnsupportedRegime)
            << "NaN off the arm exactly in the unsupported regime";
      }
    }
  }
  std::printf("[AmericanExerciseRegion] ALO identity: %zu calls x 4 pricers, classes cont=%zu "
              "plat=%zu euro=%zu expiry=%zu zerovol=%zu unsupported=%zu\n",
              calls, seen[0], seen[1], seen[2], seen[3], seen[4], seen[5]);
  for (std::size_t c = 0; c < seen.size(); ++c) {
    EXPECT_GT(seen[c], std::size_t{0}) << "coverage: class " << c;
  }
}

// ── 4. Monotone in sigma; the plateau is flat ─────────────────────────────────────

[[nodiscard]] std::vector<Contract> sigma_sweep_contracts() {
  std::vector<Contract> out;
  constexpr double S = 100.0;
  for (const double T : {0.5, 2.0, 5.0}) {
    for (const double K : {90.0, 110.0, 128.989, 150.0}) {
      for (const auto &rq : std::array<std::array<double, 2>, 3>{
               {{0.05, 0.015}, {0.02, -0.03}, {0.05, 0.10}}}) {
        out.push_back(Contract{S, K, T, 0.0, rq[0], rq[1], Side::Put});
      }
    }
    for (const double K : {111.0, 90.0, 76.0, 62.5}) {
      for (const auto &rq : std::array<std::array<double, 2>, 2>{{{0.05, 0.10}, {0.01, 0.06}}}) {
        out.push_back(Contract{S, K, T, 0.0, rq[0], rq[1], Side::Call});
      }
    }
  }
  return out;
}

TEST(AmericanExerciseRegion, PriceIsMonotoneInSigmaAndFlatOnThePlateau) {
  int floor_dips = 0;
  int plateau_points = 0;
  int continuation_steps = 0;
  for (const Contract &c : sigma_sweep_contracts()) {
    double prev_i = kNaN;
    double prev_f = kNaN;
    AlExerciseClass prev_cls = AlExerciseClass::UnsupportedRegime;
    for (int k = 0; k <= 119; ++k) {
      const double sg = 0.01 + 0.01 * k;
      const Result<AlRegionPrice> pi =
          classified(c.S, c.K, c.T, sg, c.r, c.q, c.side, AlExerciseRegion::Intrinsic);
      const Result<AlRegionPrice> pf =
          classified(c.S, c.K, c.T, sg, c.r, c.q, c.side, AlExerciseRegion::KernelFloor);
      ASSERT_TRUE(pi.has_value() && pf.has_value() && pi->priced()) << "K " << c.K << " s " << sg;
      if (pi->cls == AlExerciseClass::ExercisePlateau) {
        ++plateau_points;
        EXPECT_TRUE(same_bits(pi->price, intrinsic_of(c.S, c.K, c.side)));
      }
      if (std::isfinite(prev_i)) {
        // Under Intrinsic the whole map is non-decreasing to the noise floor: flat on the
        // plateau, rising through the edge (intrinsic -> kernel floor >= intrinsic), rising
        // in the continuation region.
        EXPECT_GE(pi->price, prev_i - kNoiseFloor * std::max(1.0, prev_i))
            << (c.side == Side::Put ? "put" : "call") << " K " << c.K << " T " << c.T
            << " sigma " << sg << " " << to_string(prev_cls) << " -> " << to_string(pi->cls);
        if (prev_cls == AlExerciseClass::Continuation &&
            pi->cls == AlExerciseClass::Continuation) {
          ++continuation_steps;
        }
        if (pf->price < prev_f - kNoiseFloor * std::max(1.0, prev_f)) {
          ++floor_dips;
        }
      }
      prev_i = pi->price;
      prev_f = pf->price;
      prev_cls = pi->cls;
    }
  }
  std::printf("[AmericanExerciseRegion] sigma sweep: %d plateau points, %d continuation steps, "
              "%d KernelFloor dips (0 under Intrinsic)\n",
              plateau_points, continuation_steps, floor_dips);
  EXPECT_GT(floor_dips, 0) << "precondition: the kernel floor dips somewhere in this sweep";
  EXPECT_GT(plateau_points, 100);
  EXPECT_GT(continuation_steps, 1000);
}

// ── 5. The genuine exercise plateau ──────────────────────────────────────────────

TEST(AmericanExerciseRegion, ExercisePlateauIsExactIntrinsicWithZeroVegaAndUnitDelta) {
  struct Case {
    Contract c;
    std::array<double, 4> sigmas;
  };
  const std::array<Case, 4> cases{{
      {{100.0, 128.989, 2.0, 0.0, 0.05, 0.015, Side::Put}, {0.03, 0.06, 0.10, 0.13}},
      {{100.0, 160.0, 1.0, 0.0, 0.05, 0.02, Side::Put}, {0.10, 0.15, 0.20, 0.25}},
      {{160.0, 100.0, 1.0, 0.0, 0.02, 0.08, Side::Call}, {0.05, 0.10, 0.15, 0.20}},
      {{100.0, 120.0, 0.5, 0.0, 0.0, -0.02, Side::Put}, {0.02, 0.04, 0.06, 0.08}},
  }};
  int checked = 0;
  for (const Case &cs : cases) {
    const Contract &c = cs.c;
    const double intr = intrinsic_of(c.S, c.K, c.side);
    for (const double sg : cs.sigmas) {
      constexpr double hs = 1.0e-4;
      const double hS = 1.0e-4 * c.S;
      const auto at = [&](double S, double sigma) {
        return classified(S, c.K, c.T, sigma, c.r, c.q, c.side, AlExerciseRegion::Intrinsic);
      };
      const Result<AlRegionPrice> p0 = at(c.S, sg);
      const Result<AlRegionPrice> pu = at(c.S, sg + hs);
      const Result<AlRegionPrice> pd = at(c.S, sg - hs);
      const Result<AlRegionPrice> su = at(c.S + hS, sg);
      const Result<AlRegionPrice> sd = at(c.S - hS, sg);
      ASSERT_TRUE(p0 && pu && pd && su && sd);
      ASSERT_EQ(p0->cls, AlExerciseClass::ExercisePlateau) << "K " << c.K << " sigma " << sg;
      ASSERT_EQ(pu->cls, AlExerciseClass::ExercisePlateau);
      ASSERT_EQ(pd->cls, AlExerciseClass::ExercisePlateau);
      ASSERT_EQ(su->cls, AlExerciseClass::ExercisePlateau);
      ASSERT_EQ(sd->cls, AlExerciseClass::ExercisePlateau);
      EXPECT_EQ(std::string_view(to_string(p0->cls)), "exercise_plateau");
      EXPECT_TRUE(same_bits(p0->price, intr));
      // vega == 0 EXACTLY: both bumps are the same intrinsic bits.
      EXPECT_EQ((pu->price - pd->price) / (2.0 * hs), 0.0) << "K " << c.K << " sigma " << sg;
      // delta = -1 (put) / +1 (call), gamma = 0, to the rounding of a 1e-4 S bump.
      const double delta = (su->price - sd->price) / (2.0 * hS);
      const double gamma = (su->price - 2.0 * p0->price + sd->price) / (hS * hS);
      EXPECT_NEAR(delta, c.side == Side::Put ? -1.0 : 1.0, 1.0e-9);
      EXPECT_NEAR(gamma, 0.0, 1.0e-6);
      ++checked;
    }
  }
  EXPECT_EQ(checked, 16);
}

// ── 6. The plateau edge is continuous ────────────────────────────────────────────

// The converged Andersen-Lake reference (lane B0's comparator uncertainty scheme: 32
// nodes, quadrature 64/64, 16 sweeps, tol 1e-14).
[[nodiscard]] AlOpts converged_reference() {
  AlOpts o;
  o.n_collocation = 32;
  o.n_quadrature = 64;
  o.n_quad_price = 64;
  o.max_newton_iter = 16;
  o.tol = 1.0e-14;
  o.profile = atx::vol::AlProfile::Reference;
  return o;
}

// The two sides of a plateau edge: the plateau-side and continuation-side evaluations and
// the parameter values they were taken at (neighbouring doubles).
struct Edge {
  AlRegionPrice in, out;
  double x_in, x_out;
};

// Bisects the class flip of `eval(x)` on [lo, hi] (classes at the two ends differ) down to
// two neighbouring doubles.
template <class Eval>
[[nodiscard]] std::optional<Edge> edge_pair(Eval &&eval, double lo, double hi) {
  Result<AlRegionPrice> a = eval(lo);
  Result<AlRegionPrice> b = eval(hi);
  if (!a || !b || a->cls == b->cls) {
    return std::nullopt;
  }
  for (int it = 0; it < 200 && std::nextafter(lo, hi) != hi; ++it) {
    const double mid = 0.5 * (lo + hi);
    Result<AlRegionPrice> m = eval(mid);
    if (!m) {
      return std::nullopt;
    }
    if (m->cls == a->cls) {
      lo = mid;
      a = m;
    } else {
      hi = mid;
      b = m;
    }
  }
  if (a->cls == AlExerciseClass::ExercisePlateau) {
    return Edge{*a, *b, lo, hi};
  }
  return Edge{*b, *a, hi, lo};
}

// The accurate preset's KernelFloor error at the plateau edge: measured 5.799e-7 *
// K * max(1, T)^2 on this test's 48 edges (call K 90, T 2, the sigma edge), each one
// reference-backed in the test below. It exceeds the AlMonotone grid's in-region maximum
// (5.011e-7): the edge, where the premium is smallest relative to its quadrature error, is
// the preset's worst in-region cell. Pinned at the 2-figure round-up.
constexpr double kAccurateEdgeClass = 5.8e-7;

TEST(AmericanExerciseRegion, PlateauEdgeIsContinuousInSpotAndSigmaWithinTheErrorClass) {
  int edges = 0;
  double worst = 0.0;
  const AlOpts ref = converged_reference();
  for (const double T : {0.5, 2.0, 5.0}) {
    std::vector<Contract> cs;
    for (const double K : {110.0, 128.989, 150.0}) {
      cs.push_back(Contract{100.0, K, T, 0.15, 0.05, 0.015, Side::Put});
      cs.push_back(Contract{100.0, K, T, 0.15, 0.02, -0.03, Side::Put});
    }
    for (const double K : {90.0, 66.7}) {
      cs.push_back(Contract{100.0, K, T, 0.15, 0.05, 0.10, Side::Call});
    }
    for (const Contract &c : cs) {
      const double scale = c.K * std::max(1.0, c.T * c.T);
      // In spot: plateau deep in the money, continuation out of it.
      const double s_itm = (c.side == Side::Put) ? 0.2 * c.K : 5.0 * c.K;
      const auto in_spot = [&](double S) {
        return classified(S, c.K, c.T, c.sigma, c.r, c.q, c.side, AlExerciseRegion::Intrinsic);
      };
      // In sigma: the plateau at low sigma (the boundary recedes as sigma grows).
      const auto in_sigma = [&](double sg) {
        return classified(c.S, c.K, c.T, sg, c.r, c.q, c.side, AlExerciseRegion::Intrinsic);
      };
      const std::optional<Edge> es = edge_pair(in_spot, s_itm, c.K);
      const std::optional<Edge> ev = edge_pair(in_sigma, 0.01, 1.5);
      for (int axis = 0; axis < 2; ++axis) {
        const std::optional<Edge> &e = (axis == 0) ? es : ev;
        if (!e) {
          continue;
        }
        const double S_out = (axis == 0) ? e->x_out : c.S;
        const double sg_out = (axis == 0) ? c.sigma : e->x_out;
        const double intr_out = intrinsic_of(S_out, c.K, c.side);
        const double jump = e->out.price - e->in.price;
        // The EXACT value is continuous at the edge (smooth pasting): the converged
        // reference at the continuation-side point is the intrinsic to its resolution...
        const Result<double> p_ref =
            andersen_lake(S_out, c.K, c.T, sg_out, c.r, c.q, c.side, ref);
        ASSERT_TRUE(p_ref.has_value());
        EXPECT_LE(std::abs(*p_ref - intr_out), 1.0e-9 * scale)
            << (c.side == Side::Put ? "put" : "call") << " K " << c.K << " T " << T;
        // ...so the jump is the preset's own error there, and the Intrinsic rule adds none.
        EXPECT_NEAR(jump, e->out.price - *p_ref, 1.0e-9 * scale);
        EXPECT_GE(e->out.price, intr_out) << "the floor never prices under intrinsic";
        EXPECT_LE(std::abs(jump), kAccurateEdgeClass * scale)
            << (c.side == Side::Put ? "put" : "call") << " K " << c.K << " T " << T
            << (axis == 0 ? " spot" : " sigma") << " edge, jump " << jump;
        worst = std::max(worst, std::abs(jump) / scale);
        ++edges;
      }
    }
  }
  std::printf("[AmericanExerciseRegion] plateau edges: %d, worst jump / (K max(1,T)^2) = %.4e\n",
              edges, worst);
  EXPECT_GE(edges, 30);
  EXPECT_GT(worst, 0.5 * kAccurateEdgeClass) << "the pin is the measured class, not slack";
}

// ── 7. Price limits ───────────────────────────────────────────────────────────────

TEST(AmericanExerciseRegion, PriceLimitsExpirySigmaInfinityAndDeepMoneyness) {
  // T -> 0: the payoff exactly at T <= kAlExpiryLimitT, and the time value vanishes like
  // sigma sqrt(T) on the way there.
  for (const Side side : {Side::Put, Side::Call}) {
    for (const double K : {80.0, 100.0, 120.0}) {
      const double intr = std::max(0.0, intrinsic_of(100.0, K, side));
      for (const double T : {0.0, 1.0e-13, kAlExpiryLimitT}) {
        const Result<AlRegionPrice> p =
            classified(100.0, K, T, 0.2, 0.05, 0.02, side, AlExerciseRegion::Intrinsic);
        ASSERT_TRUE(p.has_value());
        EXPECT_EQ(p->cls, AlExerciseClass::ExpiryLimit);
        EXPECT_TRUE(same_bits(p->price, intr));
      }
      double prev_tv = kInf;
      for (const double T : {1.0e-2, 1.0e-4, 1.0e-6, 1.0e-8, 1.0e-10}) {
        const Result<AlRegionPrice> p =
            classified(100.0, K, T, 0.2, 0.05, 0.02, side, AlExerciseRegion::Intrinsic);
        ASSERT_TRUE(p.has_value() && p->priced());
        const double tv = p->price - intr;
        EXPECT_GE(tv, 0.0);
        EXPECT_LE(tv, 100.0 * 0.2 * std::sqrt(T)) << "K " << K << " T " << T;
        EXPECT_LE(tv, prev_tv) << "K " << K << " T " << T;
        prev_tv = tv;
      }
    }
  }
  // sigma -> infinity: the put rises toward its strike, the call toward its spot, inside
  // the exact no-arbitrage bounds.
  for (const Side side : {Side::Put, Side::Call}) {
    constexpr double cap = 100.0; // the put's K and the call's S (S = K = 100 here)
    double prev = 0.0;
    double first = kNaN;
    for (const double sg : {0.5, 1.0, 2.0, 3.0, 4.0}) {
      const Result<AlRegionPrice> p =
          classified(100.0, 100.0, 1.0, sg, 0.05, 0.02, side, AlExerciseRegion::Intrinsic);
      ASSERT_TRUE(p.has_value() && p->priced()) << "sigma " << sg;
      const double F = 100.0 * std::exp(0.03);
      const double euro = atx::vol::black76_price(F, 100.0, 1.0, sg, std::exp(-0.05), side);
      EXPECT_GE(p->price, euro * (1.0 - 1.0e-12)) << "sigma " << sg;
      EXPECT_LE(p->price, cap) << "sigma " << sg;
      EXPECT_GE(p->price, prev) << "sigma " << sg;
      if (std::isnan(first)) {
        first = p->price;
      }
      prev = p->price;
    }
    EXPECT_LT(cap - prev, cap - first) << "the limit is approached";
    EXPECT_GT(prev, 0.9 * cap);
  }
  // Deep ITM put -> K - S exactly (and the mirrored deep ITM call with q > r -> S - K).
  const std::array<Contract, 3> deep{{
      {5.0, 100.0, 1.0, 0.3, 0.05, 0.02, Side::Put},
      {1.0, 100.0, 5.0, 0.6, 0.02, -0.03, Side::Put},
      {100.0, 5.0, 1.0, 0.3, 0.02, 0.05, Side::Call},
  }};
  for (const Contract &c : deep) {
    const double intr = intrinsic_of(c.S, c.K, c.side);
    const Result<AlRegionPrice> pi =
        classified(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, AlExerciseRegion::Intrinsic);
    const Result<AlRegionPrice> pf =
        classified(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, AlExerciseRegion::KernelFloor);
    ASSERT_TRUE(pi && pf);
    EXPECT_EQ(pi->cls, AlExerciseClass::ExercisePlateau);
    EXPECT_EQ(pf->cls, AlExerciseClass::ExercisePlateau);
    EXPECT_TRUE(same_bits(pi->price, intr));
    EXPECT_GE(pf->price, intr);
    EXPECT_LE(pf->price - intr, kAccurateClass * c.K * std::max(1.0, c.T * c.T));
  }
  // Deep OTM: continuation, worth (almost) nothing and never negative.
  for (const Side side : {Side::Put, Side::Call}) {
    const double K = (side == Side::Put) ? 10.0 : 1000.0;
    const Result<AlRegionPrice> p =
        classified(100.0, K, 1.0, 0.2, 0.05, 0.02, side, AlExerciseRegion::Intrinsic);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->cls, AlExerciseClass::Continuation);
    EXPECT_GE(p->price, 0.0);
    EXPECT_LT(p->price, 1.0e-12);
  }
}

// ── 8. Calls with q > r, puts across rate signs ─────────────────────────────────────

TEST(AmericanExerciseRegion, CallsWithYieldAboveRateAndPutsAcrossRateSigns) {
  struct Row {
    Side side;
    double r, q;
    AlExerciseClass deep_itm; // the class of a deep ITM, long-dated contract
  };
  const std::array<Row, 11> rows{{
      {Side::Put, 0.05, 0.02, AlExerciseClass::ExercisePlateau},   // r > 0
      {Side::Put, 0.03, 0.08, AlExerciseClass::ExercisePlateau},   // r > 0, q > r
      {Side::Put, 0.0, -0.02, AlExerciseClass::ExercisePlateau},   // r = 0 > q: one boundary
      {Side::Put, 0.0, 0.02, AlExerciseClass::European},           // r = 0 <= q
      {Side::Put, -0.01, 0.01, AlExerciseClass::European},         // r < 0 <= q
      {Side::Put, -0.01, -0.03, AlExerciseClass::UnsupportedRegime}, // q < r < 0
      {Side::Call, 0.02, 0.06, AlExerciseClass::ExercisePlateau},  // q > r > 0
      {Side::Call, -0.02, 0.03, AlExerciseClass::ExercisePlateau}, // q > 0 > r
      {Side::Call, -0.02, 0.0, AlExerciseClass::ExercisePlateau},  // q = 0 > r: one boundary
      {Side::Call, 0.04, -0.01, AlExerciseClass::European},        // q < 0 <= r
      {Side::Call, -0.03, -0.01, AlExerciseClass::UnsupportedRegime}, // r < q < 0
  }};
  for (const Row &row : rows) {
    const double S = 100.0;
    const double K = (row.side == Side::Put) ? 1000.0 : 10.0;
    for (const double sg : {0.1, 0.3}) {
      const Result<AlRegionPrice> pi =
          classified(S, K, 2.0, sg, row.r, row.q, row.side, AlExerciseRegion::Intrinsic);
      ASSERT_TRUE(pi.has_value()) << "r " << row.r << " q " << row.q;
      EXPECT_EQ(pi->cls, row.deep_itm) << (row.side == Side::Put ? "put" : "call") << " r "
                                       << row.r << " q " << row.q;
      const Result<double> legacy = andersen_lake(S, K, 2.0, sg, row.r, row.q, row.side,
                                                  std::nullopt);
      switch (pi->cls) {
      case AlExerciseClass::ExercisePlateau:
        EXPECT_TRUE(same_bits(pi->price, intrinsic_of(S, K, row.side)));
        ASSERT_TRUE(legacy.has_value());
        EXPECT_GE(*legacy, pi->price);
        break;
      case AlExerciseClass::European: {
        ASSERT_TRUE(legacy.has_value());
        EXPECT_TRUE(same_bits(pi->price, *legacy));
        const double F = S * std::exp((row.r - row.q) * 2.0);
        EXPECT_TRUE(same_bits(pi->price, atx::vol::black76_price(F, K, 2.0, sg,
                                                                 std::exp(-row.r * 2.0),
                                                                 row.side)));
        break;
      }
      case AlExerciseClass::UnsupportedRegime:
        EXPECT_FALSE(pi->priced());
        EXPECT_FALSE(legacy.has_value());
        break;
      case AlExerciseClass::Continuation:
      case AlExerciseClass::ExpiryLimit:
      case AlExerciseClass::ZeroVolLimit:
        ADD_FAILURE() << "unexpected class " << to_string(pi->cls);
        break;
      }
      // A call is its internal put, bit for bit, class and boundary included, wherever the
      // kernel runs that put (the European arm prices the call's own Black-76 instead).
      if (row.side == Side::Call && pi->cls != AlExerciseClass::European) {
        const Result<AlRegionPrice> mp =
            classified(K, S, 2.0, sg, row.q, row.r, Side::Put, AlExerciseRegion::Intrinsic);
        ASSERT_TRUE(mp.has_value());
        EXPECT_EQ(mp->cls, pi->cls);
        EXPECT_TRUE(same_bits(mp->price, pi->price) ||
                    (std::isnan(mp->price) && std::isnan(pi->price)));
        EXPECT_TRUE(same_bits(mp->boundary, pi->boundary) ||
                    (std::isnan(mp->boundary) && std::isnan(pi->boundary)));
      }
    }
  }
}

// ── 9. The two-boundary (double-continuation) regime is typed, never priced ───────

TEST(AmericanExerciseRegion, DoubleContinuationRegimeServesNoSilentPriceOnAnyRoute) {
  struct Carry {
    Side side;
    double r, q;
  };
  const std::array<Carry, 6> carries{{
      {Side::Put, -0.001, -0.002},
      {Side::Put, -0.01, -0.03},
      {Side::Put, -0.05, -0.15},
      {Side::Call, -0.002, -0.001},
      {Side::Call, -0.03, -0.01},
      {Side::Call, -0.15, -0.05},
  }};
  const std::array<std::optional<AlOpts>, 3> presets{std::nullopt, atx::vol::al_fast_opts(),
                                                     atx::vol::al_bulk_opts()};
  int typed = 0;
  std::vector<double> bS, bK, bT, bs, br, bq;
  for (const Carry &c : carries) {
    for (const double T : {0.01, 0.5, 2.0, 10.0}) {
      for (const double sg : {1.0e-6, 0.05, 0.3, 1.0}) {
        for (const double m : {0.5, 0.9, 1.0, 1.1, 2.0}) {
          const double S = 100.0;
          const double K = S * m;
          for (const std::optional<AlOpts> &o : presets) {
            const Result<double> legacy = andersen_lake(S, K, T, sg, c.r, c.q, c.side, o);
            const Result<double> ruled = andersen_lake(S, K, T, sg, c.r, c.q, c.side, o,
                                                       AlExerciseRegion::Intrinsic);
            ASSERT_FALSE(legacy.has_value()) << "silent price " << *legacy;
            ASSERT_FALSE(ruled.has_value()) << "silent price " << *ruled;
            EXPECT_EQ(legacy.error().code(), ErrorCode::NotImplemented);
            EXPECT_EQ(ruled.error().code(), ErrorCode::NotImplemented);
            for (const AlExerciseRegion rule :
                 {AlExerciseRegion::KernelFloor, AlExerciseRegion::Intrinsic}) {
              const Result<AlRegionPrice> cl =
                  andersen_lake_classified(S, K, T, sg, c.r, c.q, c.side, o, rule);
              ASSERT_TRUE(cl.has_value());
              EXPECT_EQ(cl->cls, AlExerciseClass::UnsupportedRegime);
              EXPECT_FALSE(cl->priced());
              EXPECT_TRUE(std::isnan(cl->price) && std::isnan(cl->boundary));
            }
            const AlExpiryGeometry g = atx::vol::prepare_al_geometry(T, c.r, c.q, o);
            const Result<double> gp = andersen_lake(g, S, K, sg, c.side);
            EXPECT_FALSE(gp.has_value()) << "geometry route served " << *gp;
            const Result<AlRegionPrice> gc =
                andersen_lake_classified(g, S, K, sg, c.side, AlExerciseRegion::Intrinsic);
            EXPECT_TRUE(!gc.has_value() || (!gc->priced() && std::isnan(gc->price)));
          }
          const Result<double> unified = atx::vol::american_price(
              S, K, T, sg, c.r, c.q, c.side, atx::vol::AmericanMethod::AndersenLake);
          EXPECT_FALSE(unified.has_value());
          const Result<double> eb = atx::vol::exercise_boundary(K, T, sg, c.r, c.q, c.side);
          ASSERT_FALSE(eb.has_value());
          EXPECT_EQ(eb.error().code(), ErrorCode::NotImplemented);
          if (c.side == Side::Put) {
            bS.push_back(S);
            bK.push_back(K);
            bT.push_back(T);
            bs.push_back(sg);
            br.push_back(c.r);
            bq.push_back(c.q);
          }
          ++typed;
        }
      }
    }
  }
  // The laned batch route (puts): every lane patches to the scalar refusal, never a price,
  // on the AVX2 and the scalar ISA alike.
  for (const atx::vol::simd::SimdIsa isa :
       {atx::vol::simd::SimdIsa::ForceAvx2, atx::vol::simd::SimdIsa::ForceScalar}) {
    std::vector<double> out(bS.size(), 0.0);
    (void)atx::vol::simd::american_put_boundary_batch(bS.data(), bK.data(), bT.data(),
                                                      bs.data(), br.data(), bq.data(),
                                                      out.data(), out.size(), isa);
    for (std::size_t i = 0; i < out.size(); ++i) {
      EXPECT_FALSE(std::isfinite(out[i])) << "batch lane " << i << " served " << out[i];
    }
  }
  EXPECT_EQ(typed, 480);
  // The two corners that ARE priced in this regime, by the documented closed forms: the
  // payoff at expiry (exact) and the sigma -> 0 limit (a different class, not this one).
  const Result<AlRegionPrice> at_expiry =
      classified(100.0, 120.0, 0.0, 0.3, -0.01, -0.03, Side::Put, AlExerciseRegion::Intrinsic);
  ASSERT_TRUE(at_expiry.has_value());
  EXPECT_EQ(at_expiry->cls, AlExerciseClass::ExpiryLimit);
  EXPECT_TRUE(same_bits(at_expiry->price, 20.0));
  const Result<AlRegionPrice> zero_vol =
      classified(100.0, 120.0, 1.0, 0.0, -0.01, -0.03, Side::Put, AlExerciseRegion::Intrinsic);
  ASSERT_TRUE(zero_vol.has_value());
  EXPECT_EQ(zero_vol->cls, AlExerciseClass::ZeroVolLimit);
}

} // namespace
