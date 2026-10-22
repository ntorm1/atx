#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_region.hpp" // AlExerciseRegion entry points
#include "atx/vol/api/simd/cpu.hpp"     // have_avx2 (the laned site needs the ISA at run time)
#include "pricing/american_boundary.hpp" // amer:: seam: the two scalar price sites

#if defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>

#include "simd/american_boundary_avx2_kernel.hpp" // the laned price site
#endif

// L3k monotone (2026-10-21). AlExerciseRegion::Intrinsic prices the exact intrinsic where
// the spot is on the exercise side of the solved boundary at tau = T, instead of the
// kernel's euro + premium floored at intrinsic, whose premium quadrature lands above
// intrinsic there by an amount that is not monotone in sigma (LEDGER 2026-10-19 trap;
// sprints/2026-10-18-L1c-al-nonmonotone-sprint.md). These tests pin: the dip exists under
// KernelFloor and is gone under Intrinsic; Intrinsic is bitwise KernelFloor outside the
// region and the exact intrinsic inside (oracle: exercise_boundary, an independent solve);
// the KernelFloor overloads are bitwise the default entry points; the in-region change is
// within the bound the L3k probe measured; the three price sites (scalar uncached, scalar
// cached, laned AVX2) agree bit for bit; validation, errors and closed forms are unchanged.

namespace {

using atx::core::ErrorCode;
using atx::core::Result;
using atx::vol::al_bulk_opts;
using atx::vol::al_fast_opts;
using atx::vol::al_fast_profile;
using atx::vol::AlExerciseRegion;
using atx::vol::AlExpiryGeometry;
using atx::vol::AlOpts;
using atx::vol::andersen_lake;
using atx::vol::exercise_boundary;
using atx::vol::prepare_al_geometry;
using atx::vol::Side;
namespace amer = atx::vol::amer;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

struct Preset {
  const char *name;
  std::optional<AlOpts> opts;
  double bound; // pinned max |P_I - P_K| / (K max(1, T)^2), from the L3k probe (2-fig round-up)
};

[[nodiscard]] std::array<Preset, 4> presets() {
  return {{{"accurate", std::nullopt, 1.2e-6},
           {"fast_profile", al_fast_profile(std::nullopt), 1.2e-6},
           {"al_fast_opts", al_fast_opts(), 1.2e-6},
           {"al_bulk_opts", al_bulk_opts(), 1.2e-6}}};
}

[[nodiscard]] double price(double S, double K, double T, double sigma, double r, double q,
                           Side side, const std::optional<AlOpts> &opts,
                           AlExerciseRegion region) {
  const Result<double> p = andersen_lake(S, K, T, sigma, r, q, side, opts, region);
  return p ? *p : kNaN;
}

// 1 when an INDEPENDENT boundary solve (exercise_boundary) puts the spot in the exercise
// region, 0 when outside (or in the European regime: no boundary), -1 when the spot sits
// within 1e-9 relative of the edge — too close to classify against a second solve (a call
// prices through the internal put with strike S, exercise_boundary solves strike K and
// reflects: the two agree only to rounding).
[[nodiscard]] int oracle_region(double S, double K, double T, double sigma, double r, double q,
                                Side side, const std::optional<AlOpts> &opts) {
  const Result<double> b = exercise_boundary(K, T, sigma, r, q, side, opts);
  if (!b || !std::isfinite(*b)) {
    return 0;
  }
  const double rel = (side == Side::Put) ? (S / *b - 1.0) : (*b / S - 1.0); // <= 0 inside
  if (std::abs(rel) < 1.0e-9) {
    return -1;
  }
  return rel <= 0.0 ? 1 : 0;
}

struct GridCell {
  Side side;
  double S, K, T, sigma, r, q;
};

// A subset of the L3k probe grid (S = 100; put K = S m, call K = S / m), so the probe's
// measured bound covers it by construction.
[[nodiscard]] std::vector<GridCell> grid() {
  std::vector<GridCell> cells;
  constexpr double S = 100.0;
  constexpr std::array<double, 3> Ts{0.5, 2.0, 5.0};
  constexpr std::array<double, 4> ms{1.0, 1.1, 1.3, 1.6};
  constexpr std::array<std::array<double, 2>, 3> rq{{{0.05, 0.02}, {0.05, 0.10}, {0.02, -0.03}}};
  constexpr std::array<double, 4> sigmas{0.05, 0.14, 0.3, 0.8};
  for (const Side side : {Side::Put, Side::Call}) {
    for (const double T : Ts) {
      for (const double m : ms) {
        for (const auto &x : rq) {
          for (const double sg : sigmas) {
            const double K = (side == Side::Put) ? S * m : S / m;
            cells.push_back(GridCell{side, S, K, T, sg, x[0], x[1]});
          }
        }
      }
    }
  }
  return cells;
}

// The ledgered contract of the 2026-10-19 trap (L1c section 5): put S 100, K 128.989, T 2.
constexpr double kLedS = 100.0;
constexpr double kLedK = 128.989;
constexpr double kLedT = 2.0;
constexpr double kLedR = 0.05;
constexpr double kLedQ = 0.015;

TEST(AlMonotone, LedgeredContractDipsUnderKernelFloorAndIsMonotoneUnderIntrinsic) {
  int dips_floor = 0;
  int dips_intrinsic = 0;
  int in_region = 0;
  double prev_floor = kNaN;
  double prev_intr = kNaN;
  for (int k = 0; k <= 398; ++k) {
    const double sg = 0.005 + 0.0025 * k;
    const double pf = price(kLedS, kLedK, kLedT, sg, kLedR, kLedQ, Side::Put, std::nullopt,
                            AlExerciseRegion::KernelFloor);
    const double pi = price(kLedS, kLedK, kLedT, sg, kLedR, kLedQ, Side::Put, std::nullopt,
                            AlExerciseRegion::Intrinsic);
    ASSERT_TRUE(std::isfinite(pf) && std::isfinite(pi)) << "sigma " << sg;
    if (same_bits(pi, kLedK - kLedS)) {
      ++in_region;
    }
    // The L1c probe's noise floor: a step is a dip when it drops by more than 1e-12 P.
    if (std::isfinite(prev_floor) && pf < prev_floor - 1.0e-12 * std::max(1.0, pf)) {
      ++dips_floor;
    }
    if (std::isfinite(prev_intr) && pi < prev_intr) {
      ++dips_intrinsic;
    }
    prev_floor = pf;
    prev_intr = pi;
  }
  EXPECT_GT(dips_floor, 0) << "precondition: the ledgered dip under KernelFloor";
  EXPECT_GT(in_region, 0) << "precondition: low sigma puts the spot in the region";
  EXPECT_EQ(dips_intrinsic, 0) << "Intrinsic must be non-decreasing in sigma";
}

TEST(AlMonotone, IntrinsicIsBitwiseKernelFloorOutsideTheRegionAndExactIntrinsicInside) {
  for (const Preset &p : presets()) {
    int inside = 0;
    int outside = 0;
    int edge = 0;
    for (const GridCell &c : grid()) {
      const double pf = price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, p.opts,
                              AlExerciseRegion::KernelFloor);
      const double pi = price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, p.opts,
                              AlExerciseRegion::Intrinsic);
      const double intrinsic = (c.side == Side::Put) ? (c.K - c.S) : (c.S - c.K);
      const int reg = oracle_region(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, p.opts);
      EXPECT_LE(pi, pf) << p.name << ": Intrinsic never prices above KernelFloor";
      if (reg == 1) {
        ++inside;
        EXPECT_TRUE(same_bits(pi, intrinsic))
            << p.name << " inside: K " << c.K << " T " << c.T << " sigma " << c.sigma << " got "
            << pi << " want " << intrinsic;
      } else if (reg == 0) {
        ++outside;
        EXPECT_TRUE(same_bits(pi, pf))
            << p.name << " outside: K " << c.K << " T " << c.T << " sigma " << c.sigma;
      } else {
        ++edge;
      }
    }
    EXPECT_GT(inside, 20) << p.name;
    EXPECT_GT(outside, 200) << p.name;
    EXPECT_LE(edge, 2) << p.name;
  }
}

TEST(AlMonotone, KernelFloorOverloadsAreBitwiseTheDefaultEntryPoints) {
  for (const Preset &p : presets()) {
    for (const GridCell &c : grid()) {
      const Result<double> dflt = andersen_lake(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, p.opts);
      const Result<double> kf = andersen_lake(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, p.opts,
                                              AlExerciseRegion::KernelFloor);
      ASSERT_TRUE(dflt && kf);
      EXPECT_TRUE(same_bits(*dflt, *kf)) << p.name;
      const AlExpiryGeometry g = prepare_al_geometry(c.T, c.r, c.q, p.opts);
      const Result<double> g_dflt = andersen_lake(g, c.S, c.K, c.sigma, c.side);
      const Result<double> g_kf =
          andersen_lake(g, c.S, c.K, c.sigma, c.side, AlExerciseRegion::KernelFloor);
      const Result<double> g_in =
          andersen_lake(g, c.S, c.K, c.sigma, c.side, AlExerciseRegion::Intrinsic);
      const Result<double> in = andersen_lake(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, p.opts,
                                              AlExerciseRegion::Intrinsic);
      ASSERT_TRUE(g_dflt && g_kf && g_in && in);
      EXPECT_TRUE(same_bits(*g_dflt, *dflt)) << p.name;
      EXPECT_TRUE(same_bits(*g_kf, *dflt)) << p.name;
      EXPECT_TRUE(same_bits(*g_in, *in)) << p.name << ": geometry Intrinsic == fresh Intrinsic";
    }
  }
}

TEST(AlMonotone, InRegionExcessWithinThePinnedBound) {
  for (const Preset &p : presets()) {
    double worst = 0.0;
    int changed = 0;
    for (const GridCell &c : grid()) {
      const double pf = price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, p.opts,
                              AlExerciseRegion::KernelFloor);
      const double pi = price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, p.opts,
                              AlExerciseRegion::Intrinsic);
      if (!same_bits(pf, pi)) {
        ++changed;
      }
      const double scale = c.K * std::max(1.0, c.T * c.T);
      worst = std::max(worst, std::abs(pi - pf) / scale);
    }
    EXPECT_GT(changed, 0) << p.name << ": precondition, the region changes some prices";
    EXPECT_LE(worst, p.bound) << p.name;
  }
}

// The three long-dated deep-ITM puts of L1c section 5 (dev200 kernel-input rows 10649 /
// 10651 / 10653, al-speed-20261017-dev200-kernel-inputs) and the ledgered contract, at a
// sigma where each spot is in the exercise region and the kernel floor prints ABOVE
// intrinsic (so a site that ignores the rule is visible).
struct SiteCase {
  double S, K, T, sigma, r, q;
};
constexpr std::array<SiteCase, 4> kSiteCases{{
    {kLedS, kLedK, kLedT, 0.14, kLedR, kLedQ},
    {84.764999389648438, 105.0, 2.2578856945037842, 0.14726887006231917, 0.051589004695415497,
     0.019473325422180075},
    {84.764999389648438, 110.0, 2.2578856945037842, 0.14688291430233497, 0.051589004695415497,
     0.019473325422180075},
    {84.764999389648438, 115.0, 2.2578856945037842, 0.14725717419700146, 0.051589004695415497,
     0.019473325422180075},
}};

TEST(AlMonotone, ThreeSitesReturnTheExactIntrinsicBitIdentically) {
  const amer::AlScheme sch = amer::scheme_from_opts(std::nullopt);
  std::vector<amer::AlWorkspace> ws(1);
  for (const SiteCase &c : kSiteCases) {
    amer::AlBoundary bnd;
    ASSERT_EQ(amer::al_solve_put_boundary(c.K, c.T, c.sigma, c.r, c.q, sch, bnd, ws[0]),
              amer::AlSolveStatus::Ok);
    const double intrinsic = c.K - c.S;
    ASSERT_LE(c.S, amer::al_boundary_now(bnd)) << "precondition: in the region, K " << c.K;
    const double floor_px =
        amer::al_put_price_from_boundary(bnd, ws[0], c.S, c.K, c.T, c.sigma, c.r, c.q);
    ASSERT_GT(floor_px, intrinsic) << "precondition: the kernel floor prints above intrinsic";
    // Site 1: scalar uncached.
    EXPECT_TRUE(same_bits(amer::al_put_price_from_boundary(bnd, ws[0], c.S, c.K, c.T, c.sigma,
                                                           c.r, c.q,
                                                           AlExerciseRegion::Intrinsic),
                          intrinsic))
        << "uncached, K " << c.K;
    // Site 2: scalar cached, both the cache-hit and the inline arm.
    for (const bool bind : {true, false}) {
      EXPECT_TRUE(same_bits(amer::al_put_price_from_boundary_cached_probe(
                                bnd, ws[0], c.S, c.K, c.T, c.sigma, c.r, c.q,
                                AlExerciseRegion::Intrinsic, bind),
                            intrinsic))
          << "cached (bind " << bind << "), K " << c.K;
      EXPECT_TRUE(same_bits(amer::al_put_price_from_boundary_cached_probe(
                                bnd, ws[0], c.S, c.K, c.T, c.sigma, c.r, c.q,
                                AlExerciseRegion::KernelFloor, bind),
                            floor_px))
          << "cached KernelFloor == uncached KernelFloor (bind " << bind << ")";
    }
    // The public entry point routes through site 1.
    const double pub = price(c.S, c.K, c.T, c.sigma, c.r, c.q, Side::Put, std::nullopt,
                             AlExerciseRegion::Intrinsic);
    EXPECT_TRUE(same_bits(pub, intrinsic)) << "andersen_lake Intrinsic, K " << c.K;
  }
#if defined(__AVX2__) && defined(__FMA__)
  if (!atx::vol::simd::have_avx2()) {
    GTEST_SKIP() << "host lacks AVX2 + FMA: the laned site cannot run";
  }
  // Site 3: the laned AVX2 kernel, all four contracts in one pack.
  alignas(32) std::array<double, 4> S{};
  std::array<double, 4> K{};
  std::array<double, 4> T{};
  std::array<double, 4> sg{};
  std::array<double, 4> r{};
  std::array<double, 4> q{};
  for (std::size_t l = 0; l < 4; ++l) {
    S[l] = kSiteCases[l].S;
    K[l] = kSiteCases[l].K;
    T[l] = kSiteCases[l].T;
    sg[l] = kSiteCases[l].sigma;
    r[l] = kSiteCases[l].r;
    q[l] = kSiteCases[l].q;
  }
  std::vector<amer::AlBoundary> pbnd(4);
  std::vector<amer::AlWorkspace> pws(4);
  atx::vol::simd::detail::PutPackBoundary pack;
  bool eligible[4] = {false, false, false, false};
  int ref = -1;
  atx::vol::simd::detail::solve_put_boundary_pack_avx2(S.data(), K.data(), T.data(), sg.data(),
                                                       r.data(), q.data(), 4, sch, pbnd.data(),
                                                       pws.data(), pack, eligible, ref);
  ASSERT_GE(ref, 0);
  const __m256d spot = _mm256_load_pd(S.data());
  alignas(32) std::array<double, 4> px_in{};
  alignas(32) std::array<double, 4> px_floor{};
  _mm256_store_pd(px_in.data(), atx::vol::simd::detail::price_put_pack_at_avx2(
                                    pack, spot, pack.Kv, pack.XMAX, AlExerciseRegion::Intrinsic));
  _mm256_store_pd(px_floor.data(), atx::vol::simd::detail::price_put_pack_avx2(pack, spot));
  for (std::size_t l = 0; l < 4; ++l) {
    ASSERT_TRUE(eligible[l]) << "lane " << l;
    const double intrinsic = K[l] - S[l];
    EXPECT_GT(px_floor[l], intrinsic) << "precondition: laned floor above intrinsic, lane " << l;
    EXPECT_TRUE(same_bits(px_in[l], intrinsic)) << "laned Intrinsic, lane " << l;
  }
#endif
}

TEST(AlMonotone, IntrinsicNeverChangesValidationErrorsOrRegimes) {
  struct Case {
    double S, K, T, sigma, r, q;
    Side side;
  };
  const std::array<Case, 12> cases{{
      {-1.0, 100.0, 1.0, 0.2, 0.05, 0.02, Side::Put},     // S <= 0
      {100.0, 0.0, 1.0, 0.2, 0.05, 0.02, Side::Put},      // K <= 0
      {100.0, 120.0, -1.0, 0.2, 0.05, 0.02, Side::Put},   // T < 0
      {100.0, 120.0, 1.0, -0.2, 0.05, 0.02, Side::Put},   // sigma < 0
      {100.0, 120.0, 1.0, 0.2, kNaN, 0.02, Side::Put},    // non-finite r
      {100.0, 120.0, 1.0, kNaN, 0.05, 0.02, Side::Put},   // NaN sigma
      {100.0, 120.0, 0.0, 0.2, 0.05, 0.02, Side::Put},    // T ~ 0: intrinsic closed form
      {100.0, 130.0, 2.0, 0.0, 0.05, 0.02, Side::Put},    // sigma ~ 0: closed form
      {100.0, 130.0, 2.0, 0.2, 0.0, 0.02, Side::Put},     // European regime (put r <= 0)
      {100.0, 70.0, 2.0, 0.2, 0.05, -0.01, Side::Call},   // European regime (call q <= 0)
      {100.0, 130.0, 2.0, 0.2, -0.01, -0.02, Side::Put},  // double continuation
      {100.0, 70.0, 2.0, 0.2, -0.02, -0.01, Side::Call},  // double continuation
  }};
  for (const Case &c : cases) {
    for (const Preset &p : presets()) {
      const Result<double> kf = andersen_lake(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, p.opts);
      const Result<double> in = andersen_lake(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, p.opts,
                                              AlExerciseRegion::Intrinsic);
      ASSERT_EQ(kf.has_value(), in.has_value()) << p.name << " S " << c.S << " T " << c.T;
      if (kf) {
        EXPECT_TRUE(same_bits(*kf, *in)) << p.name << " S " << c.S << " T " << c.T;
      } else {
        EXPECT_EQ(kf.error().code(), in.error().code()) << p.name;
      }
    }
  }
  const AlExpiryGeometry unprepared;
  // A default-constructed geometry is not prepared; both overloads refuse it the same way.
  const Result<double> a = andersen_lake(unprepared, 100.0, 120.0, 0.2, Side::Put);
  const Result<double> b =
      andersen_lake(unprepared, 100.0, 120.0, 0.2, Side::Put, AlExerciseRegion::Intrinsic);
  ASSERT_FALSE(a.has_value());
  ASSERT_FALSE(b.has_value());
  EXPECT_EQ(a.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(b.error().code(), ErrorCode::InvalidArgument);
}

} // namespace
