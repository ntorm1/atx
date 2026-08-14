// scenario_grid — Taylor scenario-matrix correctness gates.
//
// The grid computes the per-unique Greek bundle ONCE (a deduped
// PortfolioPricer::price solve against the base surfaces) and reconstructs each
// (spot%, vol) cell analytically to second order. These tests pin:
//
//   1. CenterCellIsZero            — a zero shock is exactly 0.0 (no solve noise).
//   2. MatchesTaylorTermsOnPureAxes — pure spot / vol / rate / time cells equal an
//      INDEPENDENT greeks() Taylor computation, term-for-term (same op order).
//   3. BitIdenticalAcrossThreads   — every cell is bit-equal at n_threads 1 vs 8.
//   4. FailedContractExcludedEverywhere — a missing-uid contract is counted once in
//      n_failed and contributes 0 to every cell (== the book without it).
//   5. MixedBookCrossTerms         — an off-axis cell equals the hand-summed
//      per-position Taylor from independent greeks() calls.
//
// Fixtures (make_essvi / set_of / make_pricing) mirror
// pnl_greeks_consistency_test.cpp: an eSSVI surface with positive carry so the
// American Greeks carry a genuine early-exercise premium.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/detail/aggregate_arity.hpp" // the DerivGreeks pin the 2^6 test carries
#include "atx/vol/detail/counters.hpp" // A7: the always-on sl_al_boundary_solves ledger
#include "atx/vol/detail/deriv_ref_bridge.hpp" // I-7: DerivShock / deriv_price_shocked_on_ref
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/scenario_grid.hpp"
#include "atx/vol/vol_curve.hpp"

using namespace atx::vol;

namespace {
constexpr double kInf = std::numeric_limits<double>::infinity();
} // namespace

namespace {

constexpr double kS = 100.0;
constexpr double kR = 0.043;
constexpr std::int64_t kNow = 1700000000000000000LL;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

[[nodiscard]] PricingContext make_pricing(std::uint32_t uid, double S = kS, double r = kR,
                                          std::int64_t now = kNow, AlOpts al = al_fast_opts()) {
  PricingContext pc;
  pc.S = S;
  pc.r = r;
  pc.now_ts_ns = now;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al;
  pc.uid = uid;
  return pc;
}

// eSSVI priced surface (positive carry => genuine American premium on both sides).
// `q_eff` may be negative (used by the fallback test to drive a shocked rate into the
// unsupported negative-carry regime); `al` tunes the cold pricer accuracy preset.
[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n, double S = kS, double r = kR,
                                       double q_eff = 0.02, AlOpts al = al_fast_opts()) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i);
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = kS;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-r * T)));
    ctx.push_back(SliceContext{T, kS, 0.0, q_eff, 250, 7});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid, S, r, kNow, al));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] SurfaceSet set_of(const std::vector<const PricedSurface *> &v) {
  auto ss = SurfaceSet::create(v);
  EXPECT_TRUE(ss.has_value());
  return std::move(*ss);
}

// Effective deliverable, mirroring portfolio_pricer.cpp eff_multiplier.
[[nodiscard]] double eff_mult(double m) noexcept {
  return (std::isfinite(m) && m > 0.0) ? m : 100.0;
}

// Position-scaled greeks for one position, computed INDEPENDENTLY via the public
// PricedSurface::greeks() (per-share) times the position weight — the reference
// the grid must reproduce bit-for-bit (price() sources its greeks from
// evaluate_batch, which is bit-identical to greeks()).
[[nodiscard]] AmericanGreeks scaled_greeks(const PricedSurface &surf, const Position &p) {
  auto g = surf.greeks(p.contract.K, p.contract.T, p.contract.side);
  EXPECT_TRUE(g.has_value());
  const double w = p.qty * eff_mult(p.multiplier);
  return AmericanGreeks{w * g->delta, w * g->gamma, w * g->vega,  w * g->theta, w * g->rho,
                        w * g->vanna, w * g->volga, w * g->charm, 0.0};
}

// A single Call position on uid 1.
[[nodiscard]] std::vector<Position> one_call() {
  return {Position{0, OptionContract{1, 100.0, 0.25, Side::Call}, +4.0, 100.0}};
}

// A mixed multi-position book on uid 1 (5 strikes × 2 sides, mixed qty/T/side).
[[nodiscard]] std::vector<Position> mixed_book() {
  std::vector<Position> book;
  std::uint64_t id = 0;
  for (double K : {92.0, 98.0, 100.0, 104.0, 110.0}) {
    book.push_back({id++, {1, K, 0.18, Side::Call}, +4.0, 100.0});
    book.push_back({id++, {1, K, 0.30, Side::Put}, -3.0, 100.0});
  }
  return book;
}

} // namespace

// ── 1. Zero shock is exactly zero. ────────────────────────────────────────────
TEST(ScenarioGrid, CenterCellIsZero) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});

  ScenarioGridSpec spec;
  spec.spot_pct = {-0.10, -0.05, 0.0, 0.05, 0.10};
  spec.vol_bump = {-0.04, 0.0, 0.04};
  spec.dr = 0.0;
  spec.dt = 0.0;

  auto r = scenario_grid(mixed_book(), bset, spec);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  ASSERT_EQ(r->n_spot, 5u);
  ASSERT_EQ(r->n_vol, 3u);
  ASSERT_EQ(r->n_cells(), 15u);

  // Center cell: spot_pct index 2 (=0.0), vol_bump index 1 (=0.0).
  const std::size_t center = 2u * r->n_vol + 1u;
  EXPECT_EQ(r->pnl[center], 0.0);
  EXPECT_EQ(r->route[center], static_cast<std::uint8_t>(ScenarioRoute::Taylor));
  // Every position priced -> no failures.
  EXPECT_EQ(r->n_failed, 0u);
  EXPECT_GT(r->n_ok, 0u);
}

// ── 2. Pure axes match an independent greeks() Taylor computation. ────────────
TEST(ScenarioGrid, MatchesTaylorTermsOnPureAxes) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});
  const std::vector<Position> book = one_call();
  const AmericanGreeks g = scaled_greeks(base, book.front());
  const double S = base.pricing().S;

  // Pure spot axis (vol_bump = {0}, dr = dt = 0): only delta/gamma light up.
  {
    ScenarioGridSpec spec;
    spec.spot_pct = {-0.08, -0.02, 0.0, 0.03, 0.09};
    spec.vol_bump = {0.0};
    spec.taylor_radius_spot = kInf; // pin all-Taylor: this test asserts the Taylor kernel
    spec.taylor_radius_vol = kInf;
    auto r = scenario_grid(book, bset, spec);
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    for (std::size_t i = 0; i < spec.spot_pct.size(); ++i) {
      const double dS = spec.spot_pct[i] * S;
      const double expected = scenario_taylor_leg(g, dS, 0.0, 0.0, 0.0);
      EXPECT_TRUE(bits_equal(r->pnl[i], expected)) << "spot cell " << i;
    }
  }

  // Pure vol axis (spot_pct = {0}): only vega/volga light up.
  {
    ScenarioGridSpec spec;
    spec.spot_pct = {0.0};
    spec.vol_bump = {-0.05, -0.01, 0.0, 0.02, 0.06};
    spec.taylor_radius_spot = kInf; // pin all-Taylor: this test asserts the Taylor kernel
    spec.taylor_radius_vol = kInf;
    auto r = scenario_grid(book, bset, spec);
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    for (std::size_t j = 0; j < spec.vol_bump.size(); ++j) {
      const double expected = scenario_taylor_leg(g, 0.0, spec.vol_bump[j], 0.0, 0.0);
      EXPECT_TRUE(bits_equal(r->pnl[j], expected)) << "vol cell " << j;
    }
  }

  // Pure rate/time scalars with dS = dvol = 0: theta/rho carry, charm term is 0.
  {
    ScenarioGridSpec spec;
    spec.spot_pct = {0.0};
    spec.vol_bump = {0.0};
    spec.dr = 1e-4;
    spec.dt = 1.0 / 365.0;
    spec.taylor_radius_spot = kInf; // dr/dt don't route; pin all-Taylor anyway for clarity
    spec.taylor_radius_vol = kInf;
    auto r = scenario_grid(book, bset, spec);
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    const double expected = scenario_taylor_leg(g, 0.0, 0.0, spec.dt, spec.dr);
    // charm term (charm*dS*dt) is 0 because dS = 0.
    const double no_charm = g.theta * spec.dt + g.rho * spec.dr;
    EXPECT_TRUE(bits_equal(r->pnl[0], expected)) << "rate/time cell";
    EXPECT_TRUE(bits_equal(expected, no_charm)) << "charm term should vanish at dS=0";
    EXPECT_NE(r->pnl[0], 0.0);
  }
}

// ── 3. Bit-identical across thread counts. ────────────────────────────────────
TEST(ScenarioGrid, BitIdenticalAcrossThreads) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});

  ScenarioGridSpec spec;
  spec.spot_pct = {-0.10, -0.08, -0.06, -0.04, -0.02, 0.0, 0.02, 0.04, 0.06, 0.08, 0.10};
  spec.vol_bump = {-0.05, -0.04, -0.03, -0.02, -0.01, 0.0, 0.01, 0.02, 0.03, 0.04, 0.05};
  spec.dr = 5e-4;
  spec.dt = 3.0 / 365.0;

  spec.n_threads = 1;
  auto r1 = scenario_grid(mixed_book(), bset, spec);
  spec.n_threads = 8;
  auto r8 = scenario_grid(mixed_book(), bset, spec);
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r8.has_value());
  ASSERT_EQ(r1->n_cells(), 121u);
  ASSERT_EQ(r1->n_cells(), r8->n_cells());
  for (std::size_t c = 0; c < r1->n_cells(); ++c) {
    EXPECT_TRUE(bits_equal(r1->pnl[c], r8->pnl[c])) << "cell " << c;
    EXPECT_EQ(r1->route[c], r8->route[c]) << "cell " << c;
  }
  EXPECT_EQ(r1->n_ok, r8->n_ok);
  EXPECT_EQ(r1->n_failed, r8->n_failed);
}

// ── 4. A failed (missing-uid) contract is excluded everywhere + counted once. ─
TEST(ScenarioGrid, FailedContractExcludedEverywhere) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base}); // only uid 1 registered

  ScenarioGridSpec spec;
  spec.spot_pct = {-0.06, 0.0, 0.06};
  spec.vol_bump = {-0.03, 0.0, 0.03};
  spec.dr = 2e-4;
  spec.dt = 2.0 / 365.0;

  // Book with valid uid-1 positions PLUS one position on uid 7 (no surface).
  std::vector<Position> good = mixed_book();
  std::vector<Position> with_bad = good;
  with_bad.push_back({999, {7, 105.0, 0.22, Side::Call}, +2.0, 100.0});

  auto r_good = scenario_grid(good, bset, spec);
  auto r_bad = scenario_grid(with_bad, bset, spec);
  ASSERT_TRUE(r_good.has_value());
  ASSERT_TRUE(r_bad.has_value());

  // The extra contract is a distinct unique -> exactly one more failed lane.
  EXPECT_EQ(r_bad->n_failed, 1u);
  EXPECT_EQ(r_good->n_failed, 0u);
  EXPECT_EQ(r_bad->n_ok, r_good->n_ok);

  // Every cell is identical: the missing-uid position contributed 0 everywhere.
  ASSERT_EQ(r_good->n_cells(), r_bad->n_cells());
  for (std::size_t c = 0; c < r_good->n_cells(); ++c) {
    EXPECT_TRUE(bits_equal(r_good->pnl[c], r_bad->pnl[c])) << "cell " << c;
  }
}

// ── 5. Off-axis cell equals hand-summed per-position Taylor. ──────────────────
TEST(ScenarioGrid, MixedBookCrossTerms) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});
  const std::vector<Position> book = mixed_book();
  const double S = base.pricing().S;

  ScenarioGridSpec spec;
  spec.spot_pct = {-0.07, 0.0, 0.05};
  spec.vol_bump = {-0.02, 0.0, 0.03};
  spec.dr = 3e-4;
  spec.dt = 5.0 / 365.0;
  spec.taylor_radius_spot = kInf; // pin all-Taylor: this test asserts the Taylor kernel
  spec.taylor_radius_vol = kInf;

  auto r = scenario_grid(book, bset, spec);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();

  // Off-axis cell: i_spot = 2 (+0.05), j_vol = 2 (+0.03) — all four axes nonzero.
  const std::size_t i_spot = 2;
  const std::size_t j_vol = 2;
  const double dS = spec.spot_pct[i_spot] * S;
  const double dvol = spec.vol_bump[j_vol];

  double expected = 0.0;
  for (const Position &p : book) {
    const AmericanGreeks g = scaled_greeks(base, p);
    expected += scenario_taylor_leg(g, dS, dvol, spec.dt, spec.dr);
  }
  const std::size_t cell = i_spot * r->n_vol + j_vol;
  EXPECT_TRUE(bits_equal(r->pnl[cell], expected))
      << "off-axis cell " << cell << " got " << r->pnl[cell] << " expected " << expected;
  EXPECT_NE(r->pnl[cell], 0.0);
}

// ── Guard: empty axes are rejected. ───────────────────────────────────────────
TEST(ScenarioGrid, EmptyAxisRejected) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});
  ScenarioGridSpec spec;
  spec.spot_pct = {};
  spec.vol_bump = {0.0};
  auto r = scenario_grid(mixed_book(), bset, spec);
  EXPECT_FALSE(r.has_value());
}

// Checked-shape arithmetic is pinned without asking the allocator for an
// impossible vector. scenario_grid uses this seam for cells, Exact-price slots,
// and executor tasks before evaluating each multiplication.
TEST(ScenarioGrid, ShapeProductsRejectOverflowWithoutAllocation) {
  constexpr std::size_t max = (std::numeric_limits<std::size_t>::max)();
  EXPECT_TRUE(detail::scenario_grid_product_is_representable(0u, max));
  EXPECT_TRUE(detail::scenario_grid_product_is_representable(max, 1u));
  EXPECT_TRUE(detail::scenario_grid_product_is_representable(max / 2u, 2u));
  EXPECT_FALSE(detail::scenario_grid_product_is_representable(max, 2u));
  EXPECT_FALSE(detail::scenario_grid_product_is_representable(max / 2u + 1u, 2u));
}

// ═══════════════════════════════════════════════════════════════════════════════
// C3.2 — Exact re-solve routing for large bumps.
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

// The .cpp's shocked-reprice guards — MUST match scenario_grid.cpp kSigmaFloor/kMinT.
constexpr double kSigmaFloor = 1.0e-4;
constexpr double kMinT = 1.0e-6;

// Independent replica of one Exact cell's per-share (P' - P0) for a single contract,
// using the SAME resolve + american_price the grid uses. nullopt if either solve errs.
[[nodiscard]] std::optional<double> exact_pershare(const PricedSurface &s, double K, double T,
                                                   Side side, double sp, double dvol, double dt,
                                                   double dr) {
  const auto rp = s.resolve(K, T);
  if (!rp.valid) {
    return std::nullopt;
  }
  const PricingContext &pc = s.pricing();
  const std::optional<AlOpts> opt{pc.al_opts};
  const auto p0 = american_price(pc.S, K, T, rp.sigma, rp.rate, rp.q_eff, side, pc.method, opt);
  const double Sp = pc.S * (1.0 + sp);
  const double sig = std::max(rp.sigma + dvol, kSigmaFloor);
  const double rr = rp.rate + dr;
  const double Tp = std::max(T - dt, kMinT);
  const auto pp = american_price(Sp, K, Tp, sig, rr, rp.q_eff, side, pc.method, opt);
  if (!p0.has_value() || !pp.has_value()) {
    return std::nullopt;
  }
  return *pp - *p0;
}

} // namespace

// A production-shaped sparse-Exact regression. The legacy dense scratch allocated
// all 100x100 cells x all 10,000 uniques = 100,000,000 doubles (~800 MB), even
// though only the final vol column routes Exact and only one unique priced Ok. The
// compact index owes 100 Exact rows x one successful-unique column = 100 doubles.
//
// The diagnostic is stronger and less flaky than process RSS sampling: it pins the
// exact allocation element count, while the full call proves the large shape
// completes. Serial/parallel and independent-oracle checks also pin that compact
// row/column remapping does not move a bit.
TEST(ScenarioGrid, SparseExactLargeShapeBoundsScratchAndPreservesBits) {
  constexpr std::size_t kAxis = 100;
  constexpr std::size_t kUnique = 10'000;
  constexpr std::size_t kExactCells = kAxis; // one Exact vol column x every spot row
  constexpr std::size_t kLegacyDenseSlots = kAxis * kAxis * kUnique;

  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});

  std::vector<Position> book;
  book.reserve(kUnique);
  book.push_back({0, {1, 100.0, 0.25, Side::Put}, 2.0, 100.0});
  for (std::size_t u = 1; u < kUnique; ++u) {
    // Missing uid 7 makes these unique lanes fail cheaply. Distinct strikes keep
    // them distinct contracts, reproducing the 10,000-unique allocation shape.
    book.push_back({static_cast<std::uint64_t>(u),
                    {7, 80.0 + 0.001 * static_cast<double>(u), 0.25, Side::Put},
                    1.0,
                    100.0});
  }

  ScenarioGridSpec spec;
  spec.spot_pct.reserve(kAxis);
  for (std::size_t i = 0; i < kAxis; ++i) {
    spec.spot_pct.push_back(-0.02 + 0.0004 * static_cast<double>(i));
  }
  spec.vol_bump.assign(kAxis, 0.0);
  spec.vol_bump.back() = 0.04;
  spec.taylor_radius_spot = 0.03; // every spot row remains inside
  spec.taylor_radius_vol = 0.03;  // only the final vol column exceeds

  spec.n_threads = 1;
  auto r1 = scenario_grid(book, bset, spec);
  spec.n_threads = 4;
  auto r4 = scenario_grid(book, bset, spec);
  ASSERT_TRUE(r1.has_value()) << r1.error().to_string();
  ASSERT_TRUE(r4.has_value()) << r4.error().to_string();

  ASSERT_EQ(r1->n_cells(), kAxis * kAxis);
  EXPECT_EQ(r1->n_ok, 1u);
  EXPECT_EQ(r1->n_failed, kUnique - 1u);
  EXPECT_EQ(r1->n_exact_price_scratch_slots, kExactCells);
  EXPECT_EQ(r4->n_exact_price_scratch_slots, kExactCells);
  EXPECT_EQ(kLegacyDenseSlots * sizeof(double), 800'000'000u);
  EXPECT_LT(r1->n_exact_price_scratch_slots, kLegacyDenseSlots / 100'000u);

  const AmericanGreeks g = scaled_greeks(base, book.front());
  const double weight = book.front().qty * eff_mult(book.front().multiplier);
  std::size_t exact_cells = 0;
  for (std::size_t i = 0; i < kAxis; ++i) {
    for (std::size_t j = 0; j < kAxis; ++j) {
      const std::size_t c = i * kAxis + j;
      const bool exact = j + 1u == kAxis;
      exact_cells += exact ? 1u : 0u;
      EXPECT_EQ(r1->route[c],
                static_cast<std::uint8_t>(exact ? ScenarioRoute::Exact : ScenarioRoute::Taylor));
      EXPECT_EQ(r1->route[c], r4->route[c]);
      EXPECT_TRUE(bits_equal(r1->pnl[c], r4->pnl[c])) << "cell " << c;

      double expected = 0.0;
      if (exact) {
        const auto d = exact_pershare(base, 100.0, 0.25, Side::Put, spec.spot_pct[i],
                                      spec.vol_bump[j], spec.dt, spec.dr);
        ASSERT_TRUE(d.has_value());
        expected = *d * weight;
      } else {
        expected = scenario_taylor_leg(g, spec.spot_pct[i] * base.pricing().S, spec.vol_bump[j],
                                       spec.dt, spec.dr);
      }
      EXPECT_TRUE(bits_equal(r1->pnl[c], expected)) << "cell " << c;
    }
  }
  EXPECT_EQ(exact_cells, kExactCells);
}

// ── C3.2-1. radius=inf reproduces the C3.1 all-Taylor grid byte-for-byte. ─────
TEST(ScenarioGrid, InfiniteRadiusIsByteIdenticalToTaylorOnly) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});
  const std::vector<Position> book = mixed_book();
  const double S = base.pricing().S;

  ScenarioGridSpec spec;
  spec.spot_pct = {-0.20, -0.10, 0.0, 0.10, 0.20}; // bumps that WOULD route Exact by default
  spec.vol_bump = {-0.08, 0.0, 0.08};
  spec.dr = 5e-4;
  spec.dt = 3.0 / 365.0;
  spec.taylor_radius_spot = kInf;
  spec.taylor_radius_vol = kInf;

  auto r = scenario_grid(book, bset, spec);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  EXPECT_EQ(r->n_exact_fallback_lanes, 0u);
  for (std::size_t i = 0; i < spec.spot_pct.size(); ++i) {
    for (std::size_t j = 0; j < spec.vol_bump.size(); ++j) {
      const double dS = spec.spot_pct[i] * S;
      const double dvol = spec.vol_bump[j];
      double expected = 0.0;
      for (const Position &p : book) {
        expected += scenario_taylor_leg(scaled_greeks(base, p), dS, dvol, spec.dt, spec.dr);
      }
      const std::size_t c = i * r->n_vol + j;
      EXPECT_EQ(r->route[c], static_cast<std::uint8_t>(ScenarioRoute::Taylor)) << "cell " << c;
      EXPECT_TRUE(bits_equal(r->pnl[c], expected)) << "cell " << c;
    }
  }
}

// ── C3.2-2. Route flips EXACTLY at the radius (strict >, per cell). ───────────
TEST(ScenarioGrid, RouteFlipsExactlyAtRadius) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});

  ScenarioGridSpec spec;
  spec.spot_pct = {0.04, 0.05, 0.06, -0.06}; // vs 0.05: T, T(== is not >), E, E
  spec.vol_bump = {0.02, 0.03, 0.04};        // vs 0.03: T, T(==), E
  spec.taylor_radius_spot = 0.05;
  spec.taylor_radius_vol = 0.03;

  auto r = scenario_grid(mixed_book(), bset, spec);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  for (std::size_t i = 0; i < spec.spot_pct.size(); ++i) {
    for (std::size_t j = 0; j < spec.vol_bump.size(); ++j) {
      const bool exact = std::abs(spec.spot_pct[i]) > 0.05 || std::abs(spec.vol_bump[j]) > 0.03;
      const std::size_t c = i * r->n_vol + j;
      EXPECT_EQ(r->route[c],
                static_cast<std::uint8_t>(exact ? ScenarioRoute::Exact : ScenarioRoute::Taylor))
          << "cell (" << i << "," << j << ")";
    }
  }
}

// ── C3.2-3. One forced-Exact cell == an independent direct reprice, bit-equal. ─
TEST(ScenarioGrid, ExactCellMatchesDirectReprice) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});
  const std::vector<Position> book = {{0, {1, 100.0, 0.25, Side::Call}, +4.0, 100.0}};

  ScenarioGridSpec spec;
  spec.spot_pct = {0.20}; // > radius => Exact
  spec.vol_bump = {0.05};
  spec.dr = 3e-4;
  spec.dt = 2.0 / 365.0;
  spec.taylor_radius_spot = 0.10;
  spec.taylor_radius_vol = 0.10;

  auto r = scenario_grid(book, bset, spec);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  ASSERT_EQ(r->n_cells(), 1u);
  EXPECT_EQ(r->route[0], static_cast<std::uint8_t>(ScenarioRoute::Exact));
  EXPECT_EQ(r->n_exact_fallback_lanes, 0u);

  const auto d = exact_pershare(base, 100.0, 0.25, Side::Call, 0.20, 0.05, spec.dt, spec.dr);
  ASSERT_TRUE(d.has_value());
  const double w = 4.0 * 100.0; // qty * eff_mult(100)
  EXPECT_TRUE(bits_equal(r->pnl[0], *d * w)) << r->pnl[0] << " vs " << (*d * w);
}

// ── C3.2-4. The base-reprice (P0) path == fair_value, bit-equal. ─────────────
TEST(ScenarioGrid, BaseRepriceMatchesFairValue) {
  const PricedSurface base = make_essvi(1, 5);
  for (const Position &p : mixed_book()) {
    const double K = p.contract.K;
    const double T = p.contract.T;
    const Side side = p.contract.side;
    const auto rp = base.resolve(K, T);
    ASSERT_TRUE(rp.valid);
    const PricingContext &pc = base.pricing();
    const auto p0 = american_price(pc.S, K, T, rp.sigma, rp.rate, rp.q_eff, side, pc.method,
                                   std::optional<AlOpts>{pc.al_opts});
    const auto fv = base.fair_value(K, T, side);
    ASSERT_TRUE(p0.has_value());
    ASSERT_TRUE(fv.has_value());
    EXPECT_TRUE(bits_equal(*p0, *fv)) << "K=" << K << " T=" << T;
  }
}

// ── C3.2-5. Taylor and Exact agree INSIDE the declared radii. ──────────────
TEST(ScenarioGrid, TaylorExactAgreeInsideRadius) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});
  // Axes AT/inside the default radii — every cell would route Taylor by default.
  const std::vector<double> spot = {-kDefaultTaylorRadiusSpot, -0.01, 0.0, 0.01,
                                    kDefaultTaylorRadiusSpot};
  const std::vector<double> vol = {-kDefaultTaylorRadiusVol, -0.01, 0.0, 0.01,
                                   kDefaultTaylorRadiusVol};
  double worst = 0.0;
  // Same board as ScenarioGrid.MeasureTaylorRadius (test:649-650) — wing strikes and
  // tenor extremes are where the Taylor residual peaks, so the agreement gate must be
  // measured on the SAME board the radii were derived from, not a friendlier one.
  for (double K : {80.0, 90.0, 95.0, 100.0, 105.0, 110.0, 120.0}) {
    for (double T : {0.05, 0.15, 0.25, 0.35, 0.45}) {
      for (Side side : {Side::Call, Side::Put}) {
        const std::vector<Position> book = {{0, {1, K, T, side}, 1.0, 1.0}}; // per-share
        ScenarioGridSpec st;
        st.spot_pct = spot;
        st.vol_bump = vol;
        st.taylor_radius_spot = kInf; // all-Taylor
        st.taylor_radius_vol = kInf;
        ScenarioGridSpec se = st;
        se.taylor_radius_spot = 0.0; // all-Exact (except the 0/0 center)
        se.taylor_radius_vol = 0.0;
        auto rt = scenario_grid(book, bset, st);
        auto re = scenario_grid(book, bset, se);
        ASSERT_TRUE(rt.has_value());
        ASSERT_TRUE(re.has_value());
        for (std::size_t c = 0; c < rt->n_cells(); ++c) {
          ASSERT_TRUE(std::isfinite(re->pnl[c]));
          worst = std::max(worst, std::abs(rt->pnl[c] - re->pnl[c]));
        }
      }
    }
  }
  std::printf("[TaylorExactAgreeInsideRadius] worst per-share |Taylor-Exact| = %.6f\n", worst);
  // The MEASURED $0.005 band -- see `ScenarioGrid.MeasureTaylorRadius` in this file,
  // which performs that sweep rather than restating its result -- is a PER-AXIS bound
  // (pure-spot / pure-vol sweeps).
  // With OR routing, the worst Taylor cell inside the radii is the DOUBLE CORNER
  // (|spot|=rad AND |vol|=rad simultaneously), whose combined higher-order + vanna
  // cross-term residual reaches ~$0.0091 on the friendlier {90..110}/{0.15..0.35} board
  // — above the per-axis band. On the FULL board (wing strikes 80/120, tenor extremes
  // 0.05/0.45 — same board as MeasureTaylorRadius) the double-corner residual is worse,
  // measuring $0.011063. $0.0125 is that worst case plus ~13% headroom, rounded -- which
  // is the whole derivation of this number, stated here so the bound can be re-derived
  // from the line above it rather than from a document. The board widening that produced
  // the $0.011063 figure landed in 7856019.
  EXPECT_LE(worst, 1.25e-2);
}

// ── C3.2-6. Second-order convergence: residual scales ~1/8 as h -> h/2. ──────
TEST(ScenarioGrid, SecondOrderConvergence) {
  // Higher-accuracy AL preset so the exact reprice residual is not solver-noise
  // limited at the smaller step.
  const AlOpts acc{
      .n_collocation = 12, .n_quadrature = 48, .max_newton_iter = 12, .tol = 1.0e-12};
  const PricedSurface base = make_essvi(1, 5, kS, kR, 0.02, acc);
  const SurfaceSet bset = set_of({&base});
  const double K = 100.0; // ATM-ish, away from the exercise boundary (§9.2 caveat)
  const double T = 0.35;

  auto resid = [&](double h) {
    const std::vector<Position> book = {{0, {1, K, T, Side::Call}, 1.0, 1.0}};
    ScenarioGridSpec st;
    st.spot_pct = {h};
    st.vol_bump = {0.0};
    st.taylor_radius_spot = kInf;
    st.taylor_radius_vol = kInf;
    ScenarioGridSpec se = st;
    se.taylor_radius_spot = 0.0;
    se.taylor_radius_vol = 0.0;
    auto rt = scenario_grid(book, bset, st);
    auto re = scenario_grid(book, bset, se);
    EXPECT_TRUE(rt.has_value());
    EXPECT_TRUE(re.has_value());
    return std::abs(rt->pnl[0] - re->pnl[0]);
  };

  const double h = 0.06;
  const double rh = resid(h);
  const double rh2 = resid(h / 2.0);
  std::printf("[SecondOrderConvergence] resid(h=%.3f)=%.3e resid(h/2)=%.3e ratio=%.3f\n", h, rh,
              rh2, rh2 / rh);
  ASSERT_GT(rh, 1.0e-6); // residual must dominate solver noise for the ratio to mean anything
  const double ratio = rh2 / rh;
  EXPECT_GT(ratio, 0.05);
  EXPECT_LT(ratio, 0.30); // ~1/8 for an O(h^3) 2nd-order Taylor residual, generous band
}

// ── C3.2-7. Bit-identical across thread counts WITH mixed routes. ────────────
TEST(ScenarioGrid, BitIdenticalAcrossThreadsMixedRoutes) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});

  ScenarioGridSpec spec;
  spec.spot_pct = {-0.10, -0.08, -0.06, -0.04, -0.02, 0.0, 0.02, 0.04, 0.06, 0.08, 0.10};
  spec.vol_bump = {-0.05, -0.04, -0.03, -0.02, -0.01, 0.0, 0.01, 0.02, 0.03, 0.04, 0.05};
  spec.dr = 5e-4;
  spec.dt = 3.0 / 365.0;
  // Default radii => inner cells Taylor, outer cells Exact (both routes present).

  spec.n_threads = 1;
  auto r1 = scenario_grid(mixed_book(), bset, spec);
  spec.n_threads = 8;
  auto r8 = scenario_grid(mixed_book(), bset, spec);
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r8.has_value());

  bool has_taylor = false;
  bool has_exact = false;
  for (const std::uint8_t v : r1->route) {
    has_taylor |= (v == static_cast<std::uint8_t>(ScenarioRoute::Taylor));
    has_exact |= (v == static_cast<std::uint8_t>(ScenarioRoute::Exact));
  }
  EXPECT_TRUE(has_taylor) << "test is vacuous without Taylor cells";
  EXPECT_TRUE(has_exact) << "test is vacuous without Exact cells";

  ASSERT_EQ(r1->n_cells(), 121u);
  for (std::size_t c = 0; c < r1->n_cells(); ++c) {
    EXPECT_TRUE(bits_equal(r1->pnl[c], r8->pnl[c])) << "cell " << c;
    EXPECT_EQ(r1->route[c], r8->route[c]) << "cell " << c;
  }
  EXPECT_EQ(r1->n_ok, r8->n_ok);
  EXPECT_EQ(r1->n_failed, r8->n_failed);
  EXPECT_EQ(r1->n_exact_fallback_lanes, r8->n_exact_fallback_lanes);
}

// ── C3.2-8. dt clamp + engineered exact fallback are counted, no NaN. ────────
TEST(ScenarioGrid, DtClampAndFallbackCounted) {
  // (a) Fallback: a negative-carry surface (base still prices — r>0), then a large
  // negative dr drives r' into the unsupported band (q_eff < r' <= 0) so the shocked
  // put Errs and the lane falls back to its Taylor leg (route stays Exact).
  {
    const PricedSurface s = make_essvi(1, 5, kS, kR, /*q_eff=*/-0.05);
    const SurfaceSet bset = set_of({&s});
    const std::vector<Position> book = {{0, {1, 100.0, 0.25, Side::Put}, -3.0, 100.0}};
    ScenarioGridSpec spec;
    spec.spot_pct = {0.20}; // forced Exact
    spec.vol_bump = {0.0};
    spec.dr = -0.05; // r' = 0.043 - 0.05 = -0.007 in (q_eff=-0.05, 0]
    spec.dt = 0.0;   // keep T' non-degenerate so the regime check (not intrinsic) fires
    spec.taylor_radius_spot = 0.10;
    spec.taylor_radius_vol = 0.10;
    auto r = scenario_grid(book, bset, spec);
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    EXPECT_EQ(r->route[0], static_cast<std::uint8_t>(ScenarioRoute::Exact));
    EXPECT_GT(r->n_exact_fallback_lanes, 0u);
    ASSERT_TRUE(std::isfinite(r->pnl[0]));
    // The fallback cell equals the unique's Taylor leg.
    const double dS = 0.20 * s.pricing().S;
    const double expected =
        scenario_taylor_leg(scaled_greeks(s, book.front()), dS, 0.0, 0.0, -0.05);
    EXPECT_TRUE(bits_equal(r->pnl[0], expected));
  }
  // (b) dt clamp: dt >> shortest T clamps T' to kMinT; the reprice collapses to a
  // finite intrinsic (no NaN, no fallback) on a normal positive-carry surface.
  {
    const PricedSurface s = make_essvi(1, 5);
    const SurfaceSet bset = set_of({&s});
    ScenarioGridSpec spec;
    spec.spot_pct = {0.20};
    spec.vol_bump = {0.06};
    spec.dt = 1.0; // >> shortest T (0.18) => T' clamps to kMinT
    spec.taylor_radius_spot = 0.10;
    spec.taylor_radius_vol = 0.03;
    auto r = scenario_grid(mixed_book(), bset, spec);
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    EXPECT_EQ(r->route[0], static_cast<std::uint8_t>(ScenarioRoute::Exact));
    EXPECT_EQ(r->n_exact_fallback_lanes, 0u);
    for (const double v : r->pnl) {
      EXPECT_TRUE(std::isfinite(v));
    }
  }
}

// ── C3.2-9. The measured default radii are pinned (silent change fails). ─────
TEST(ScenarioGrid, DefaultRadiiPinned) {
  // MEASURED by `ScenarioGrid.MeasureTaylorRadius` in this file: the largest pure-axis
  // bump whose worst per-share Taylor residual stays within $0.005. Both axes land on
  // 3%, and the residual jumps past the band at the next step up.
  EXPECT_EQ(kDefaultTaylorRadiusSpot, 0.03);
  EXPECT_EQ(kDefaultTaylorRadiusVol, 0.03);
  ScenarioGridSpec s;
  EXPECT_EQ(s.taylor_radius_spot, kDefaultTaylorRadiusSpot);
  EXPECT_EQ(s.taylor_radius_vol, kDefaultTaylorRadiusVol);
}

// ── C3.2-10. Measure the Taylor-valid radius. This test IS the source of the
// two `kDefaultTaylorRadius*` constants and of the $0.005 per-axis band every
// comment above cites; it sweeps rather than restating a recorded conclusion. ──
// Prints max per-share |Taylor - Exact| over the eSSVI board for a spot and a vol
// bump sweep. The declared radius (baked into kDefaultTaylorRadius*) is the largest
// bump whose worst-case per-share deviation stays <= $0.005.
TEST(ScenarioGrid, MeasureTaylorRadius) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});
  const double strikes[] = {80.0, 90.0, 95.0, 100.0, 105.0, 110.0, 120.0};
  const double tenors[] = {0.05, 0.15, 0.25, 0.35, 0.45};
  const double spot_bumps[] = {0.005, 0.01, 0.02, 0.03, 0.05, 0.07, 0.10, 0.15, 0.20};
  const double vol_bumps[] = {0.0025, 0.005, 0.01, 0.02, 0.03, 0.05, 0.08};

  auto worst_dev = [&](bool spot_axis, double b) {
    double worst = 0.0;
    for (double K : strikes) {
      for (double T : tenors) {
        for (Side side : {Side::Call, Side::Put}) {
          const std::vector<Position> book = {{0, {1, K, T, side}, 1.0, 1.0}};
          ScenarioGridSpec st;
          st.spot_pct = spot_axis ? std::vector<double>{b} : std::vector<double>{0.0};
          st.vol_bump = spot_axis ? std::vector<double>{0.0} : std::vector<double>{b};
          st.taylor_radius_spot = kInf;
          st.taylor_radius_vol = kInf;
          ScenarioGridSpec se = st;
          se.taylor_radius_spot = 0.0;
          se.taylor_radius_vol = 0.0;
          auto rt = scenario_grid(book, bset, st);
          auto re = scenario_grid(book, bset, se);
          if (rt.has_value() && re.has_value() && std::isfinite(re->pnl[0])) {
            worst = std::max(worst, std::abs(rt->pnl[0] - re->pnl[0]));
          }
        }
      }
    }
    return worst;
  };

  std::printf("\n[MeasureTaylorRadius] pure-spot sweep (fraction -> max per-share dev $):\n");
  for (double b : spot_bumps) {
    std::printf("    spot %-7.4f  %.6f\n", b, worst_dev(true, b));
  }
  std::printf("[MeasureTaylorRadius] pure-vol sweep (vol pts -> max per-share dev $):\n");
  for (double b : vol_bumps) {
    std::printf("    vol  %-7.4f  %.6f\n", b, worst_dev(false, b));
  }
  SUCCEED();
}

// ═══════════════════════════════════════════════════════════════════════════════
// A7 (GR-P3-S) — the Exact arm's boundary-solve ledger, and the value-identity
// guard that makes the drop legitimate.
// ═══════════════════════════════════════════════════════════════════════════════
//
// The Andersen-Lake exercise boundary depends on (K, T, sigma, r, q) but NOT on the
// spot S (american.cpp's "S-independence seam"). Every Exact cell of one grid shares
// K, q, T' = T - dt and r' = r + dr, so for a PUT (whose internal-put strike is K)
// the boundary is a function of the VOL-BUMP INDEX ALONE: one solve per
// (unique, vol column) prices the whole spot axis of that column, bit-for-bit.
//
// A CALL is the McDonald-Schroder internal put P(K, S', q, r'): its internal-put
// STRIKE is the shocked spot S', so its boundary moves with BOTH axes and no exact
// reuse exists. That asymmetry is pinned below deliberately — reusing a call's
// boundary across the spot axis would only be homogeneity-EXACT in R, a few ULP off
// in IEEE, which is precisely the value shift these tests forbid.

namespace {

// Whole-book ledger delta (`sl_al_boundary_solves`) for one scenario_grid call.
[[nodiscard]] std::uint64_t grid_boundary_solves(const std::vector<Position> &book,
                                                 const SurfaceSet &bset,
                                                 const ScenarioGridSpec &spec) {
  const auto before = counters::ledger::snapshot();
  auto r = scenario_grid(book, bset, spec);
  const auto after = counters::ledger::snapshot();
  EXPECT_TRUE(r.has_value());
  return (after - before).get(counters::ledger::Solve::AlBoundarySolves);
}

// The Exact ARM's own cold solves, isolated from the (route-independent) Greek
// bundle by differencing against the SAME grid with routing disabled, then removing
// the one base-reprice (P0) solve each Ok unique pays before the cell fill.
[[nodiscard]] std::uint64_t exact_arm_solves(const std::vector<Position> &book,
                                             const SurfaceSet &bset, ScenarioGridSpec spec,
                                             std::size_t n_unique) {
  ScenarioGridSpec taylor = spec;
  taylor.taylor_radius_spot = kInf;
  taylor.taylor_radius_vol = kInf;
  const std::uint64_t greeks_only = grid_boundary_solves(book, bset, taylor);
  const std::uint64_t total = grid_boundary_solves(book, bset, spec);
  EXPECT_GE(total, greeks_only + n_unique);
  return total - greeks_only - static_cast<std::uint64_t>(n_unique);
}

// A put-only book: 5 distinct strikes, one unique each.
[[nodiscard]] std::vector<Position> put_only_book() {
  std::vector<Position> book;
  std::uint64_t id = 0;
  for (double K : {92.0, 98.0, 100.0, 104.0, 110.0}) {
    book.push_back({id++, {1, K, 0.30, Side::Put}, -3.0, 100.0});
  }
  return book;
}

// A call-only book on the SAME strikes/tenor, so the two ledger readings differ
// only by the side (and therefore only by whether boundary reuse exists).
[[nodiscard]] std::vector<Position> call_only_book() {
  std::vector<Position> book;
  std::uint64_t id = 0;
  for (double K : {92.0, 98.0, 100.0, 104.0, 110.0}) {
    book.push_back({id++, {1, K, 0.18, Side::Call}, +4.0, 100.0});
  }
  return book;
}

// The all-Exact grid both ledger tests use: no zero on either axis and both radii 0,
// so every one of the 4 x 3 cells routes Exact.
[[nodiscard]] ScenarioGridSpec all_exact_spec(unsigned n_threads = 1) {
  ScenarioGridSpec spec;
  spec.spot_pct = {-0.10, -0.05, 0.05, 0.10};
  spec.vol_bump = {-0.04, 0.02, 0.06};
  spec.dr = 5e-4;
  spec.dt = 3.0 / 365.0;
  spec.n_threads = n_threads;
  spec.taylor_radius_spot = 0.0;
  spec.taylor_radius_vol = 0.0;
  return spec;
}

// A vol ladder whose SPOT axis is entirely inside the Taylor radius. `is_exact` is
// `|sp| > rad_spot || |dvol| > rad_vol`, so with the only spot shock at 0.0 a column
// routes Exact iff its own |dvol| exceeds rad_vol: 4 of these 7 columns
// ({-0.06,-0.04,+0.04,+0.06}) contain an Exact cell and 3 ({-0.02,0.0,+0.02}) contain
// none at all. A wholly-Taylor column owes NO reprice and therefore no boundary solve
// — that is what A7-1c pins. This is a legal spec that no other A7 test reaches:
// `all_exact_spec` sets both radii to 0.0, which forces every cell Exact by
// construction and hides any per-column waste.
[[nodiscard]] ScenarioGridSpec taylor_spot_axis_spec(unsigned n_threads = 1) {
  ScenarioGridSpec spec;
  spec.spot_pct = {0.0};
  spec.vol_bump = {-0.06, -0.04, -0.02, 0.0, 0.02, 0.04, 0.06};
  spec.dr = 5e-4;
  spec.dt = 3.0 / 365.0;
  spec.n_threads = n_threads;
  spec.taylor_radius_spot = 0.01; // |0.0| <= 0.01 => the spot axis never routes Exact
  spec.taylor_radius_vol = 0.03;  // 4 Exact columns, 3 wholly-Taylor columns
  return spec;
}

} // namespace

// ── A7-1c. A wholly-TAYLOR vol column costs ZERO boundary solves. ────────────
// The reuse arm hoisted the solve to (unique x vol column), but the `is_exact`
// filter that decides whether the column owes a reprice at all sits INSIDE the spot
// loop that follows the solve. On a grid whose spot axis is wholly inside the radius,
// the columns that route pure Taylor still paid one cold boundary solve per put
// unique — A7's own gate quantity moving the WRONG way, above even the pre-A7
// per-cell cost (which is `n_exact_cells * n_unique`, and is the ceiling asserted
// below). Values were never affected; the extra boundary was discarded unread.
TEST(ScenarioGrid, ExactArmWhollyTaylorVolColumnCostsNoSolve) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});
  const std::vector<Position> book = put_only_book();
  const ScenarioGridSpec spec = taylor_spot_axis_spec();

  const std::size_t n_unique = book.size(); // one position per unique
  const std::size_t n_spot = spec.spot_pct.size();
  const std::size_t n_vol = spec.vol_bump.size();

  // Anti-vacuity: the grid must really be mixed — some Exact columns (else there is
  // no Exact arm to measure) and some wholly-Taylor ones (else this is A7-1 again).
  auto probe = scenario_grid(book, bset, spec);
  ASSERT_TRUE(probe.has_value()) << probe.error().to_string();
  ASSERT_EQ(probe->n_ok, n_unique);
  ASSERT_EQ(probe->n_exact_fallback_lanes, 0u);
  std::size_t n_exact_cells = 0;
  for (const std::uint8_t rv : probe->route) {
    n_exact_cells += (rv == static_cast<std::uint8_t>(ScenarioRoute::Exact)) ? 1u : 0u;
  }
  ASSERT_EQ(n_exact_cells, 4u);
  ASSERT_EQ(probe->route.size() - n_exact_cells, 3u);

  // One solve per (put unique x EXACT vol column). With a single spot value the
  // Exact-cell count and the Exact-column count coincide, so this number is also
  // exactly the pre-A7 per-cell cost: A7 must never exceed it.
  const std::size_t n_exact_cols = n_exact_cells / n_spot;
  const std::uint64_t solves = exact_arm_solves(book, bset, spec, n_unique);
  std::printf("[A7-1c] taylor-spot-axis exact-arm solves = %llu (per-column-blind would be %zu, "
              "pre-A7 per-cell would be %zu)\n",
              static_cast<unsigned long long>(solves), n_vol * n_unique, n_exact_cells * n_unique);
  EXPECT_LE(solves, static_cast<std::uint64_t>(n_exact_cells * n_unique));
  EXPECT_EQ(solves, static_cast<std::uint64_t>(n_exact_cols * n_unique));
}

// ── A7-1. A PUT unique pays ONE boundary solve per vol column, not per cell. ──
TEST(ScenarioGrid, ExactArmPutBoundarySolvesDropToOnePerVolColumn) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});
  const std::vector<Position> book = put_only_book();
  const ScenarioGridSpec spec = all_exact_spec();

  const std::size_t n_unique = book.size(); // one position per unique
  const std::size_t n_cells = spec.spot_pct.size() * spec.vol_bump.size();
  const std::size_t n_cols = spec.vol_bump.size();

  // Every cell must actually be Exact, else the count below means nothing.
  auto probe = scenario_grid(book, bset, spec);
  ASSERT_TRUE(probe.has_value()) << probe.error().to_string();
  ASSERT_EQ(probe->n_cells(), n_cells);
  ASSERT_EQ(probe->n_ok, n_unique);
  ASSERT_EQ(probe->n_exact_fallback_lanes, 0u);
  for (const std::uint8_t rv : probe->route) {
    ASSERT_EQ(rv, static_cast<std::uint8_t>(ScenarioRoute::Exact));
  }

  const std::uint64_t solves = exact_arm_solves(book, bset, spec, n_unique);
  std::printf("[A7] put-only exact-arm solves = %llu (per-cell would be %zu)\n",
              static_cast<unsigned long long>(solves), n_cells * n_unique);
  EXPECT_LT(solves, static_cast<std::uint64_t>(n_cells * n_unique));
  EXPECT_EQ(solves, static_cast<std::uint64_t>(n_cols * n_unique));
}

// ── A7-2. The drop is STRUCTURAL: the same count at any thread count. ────────
// Reuse driven by a per-worker cache would make this number depend on how the
// cells were partitioned; reuse driven by the (unique, vol column) shape cannot.
TEST(ScenarioGrid, ExactArmSolveCountIsThreadInvariant) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});
  const std::vector<Position> book = put_only_book();
  const std::size_t n_unique = book.size();

  const std::uint64_t s1 = exact_arm_solves(book, bset, all_exact_spec(1), n_unique);
  const std::uint64_t s8 = exact_arm_solves(book, bset, all_exact_spec(8), n_unique);
  EXPECT_EQ(s1, s8);
}

// ── A7-3. A CALL unique keeps its per-cell solve (documented asymmetry). ─────
TEST(ScenarioGrid, ExactArmCallBoundarySolvesStayPerCell) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});
  const std::vector<Position> book = call_only_book();
  const ScenarioGridSpec spec = all_exact_spec();
  const std::size_t n_unique = book.size();
  const std::size_t n_cells = spec.spot_pct.size() * spec.vol_bump.size();

  const std::uint64_t solves = exact_arm_solves(book, bset, spec, n_unique);
  EXPECT_EQ(solves, static_cast<std::uint64_t>(n_cells * n_unique));
}

// ── A7-3b. The mixed-book count follows the per-side formula exactly. ────────
// n_put * n_vol_columns + n_call * n_exact_cells. Pinning it here is what lets the
// drop on any other book (e.g. the pg_observability bench's 32-put / 24-call, 8
// Exact cell, 3-column shape: 448 -> 288 Exact-arm solves) be derived rather than
// guessed.
TEST(ScenarioGrid, ExactArmMixedBookFollowsPerSideFormula) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});
  const std::vector<Position> book = mixed_book(); // 5 puts + 5 calls, distinct uniques
  const ScenarioGridSpec spec = all_exact_spec();
  const std::size_t n_unique = book.size();
  const std::size_t n_cells = spec.spot_pct.size() * spec.vol_bump.size();
  const std::size_t n_cols = spec.vol_bump.size();

  const std::uint64_t solves = exact_arm_solves(book, bset, spec, n_unique);
  std::printf("[A7] mixed-book exact-arm solves = %llu (per-cell would be %zu)\n",
              static_cast<unsigned long long>(solves), n_cells * n_unique);
  EXPECT_EQ(solves, static_cast<std::uint64_t>(5 * n_cols + 5 * n_cells));
}

// ── A7-4. VALUE IDENTITY: every Exact cell equals the cold per-cell algorithm. ─
// The oracle IS the pre-change path: american_price once per (cell x unique) with
// no reuse of any kind, reduced over positions in input order. Bitwise equality is
// required — this is what makes the solve-count drop a hoist rather than a change
// of numerics.
TEST(ScenarioGrid, ExactCellsMatchColdPerCellOracleBitwise) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});
  const std::vector<Position> book = mixed_book(); // 5 calls + 5 puts, distinct uniques
  const double S = base.pricing().S;

  ScenarioGridSpec spec = all_exact_spec();
  auto r = scenario_grid(book, bset, spec);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();

  for (std::size_t i = 0; i < spec.spot_pct.size(); ++i) {
    for (std::size_t j = 0; j < spec.vol_bump.size(); ++j) {
      const std::size_t c = i * r->n_vol + j;
      ASSERT_EQ(r->route[c], static_cast<std::uint8_t>(ScenarioRoute::Exact)) << "cell " << c;
      const double sp = spec.spot_pct[i];
      const double dvol = spec.vol_bump[j];
      double expected = 0.0;
      for (const Position &p : book) { // fixed INPUT order — the reduction contract
        const auto d = exact_pershare(base, p.contract.K, p.contract.T, p.contract.side, sp, dvol,
                                      spec.dt, spec.dr);
        if (d.has_value() && std::isfinite(*d)) {
          expected += *d * (p.qty * eff_mult(p.multiplier));
        } else {
          expected += scenario_taylor_leg(scaled_greeks(base, p), sp * S, dvol, spec.dt, spec.dr);
        }
      }
      EXPECT_TRUE(bits_equal(r->pnl[c], expected))
          << "cell " << c << " got " << r->pnl[c] << " expected " << expected;
    }
  }
}

// ── Vol-derivative leg (Task F-8, GK-G4) ────────────────────────────────────
//
// The deriv leg answers the same question the option leg does -- what does this
// book make in each (spot%, vol) cell -- for a book the option grid could not
// hold at all. Its gates below are the option leg's, restated where the deriv
// path differs, plus the one claim that is genuinely new: that the Taylor cell
// and the full reprice are two views of ONE model rather than two models.

namespace {

// A deriv position of ANY kind on `uid`, struck mid-life so the accrued and
// future legs are both live -- a fully-unaged swap would hide an error in the
// future-leg weighting, and a fully-aged one has no market risk left to shock.
//
// KIND-PARAMETERISED ON PURPOSE. Round 1 of this leg shipped six tests that all
// used a VarSwap fixture, and `price_deriv_book` memoizes VarSwap rows ONLY --
// so the entire non-memoised half of the kind space (VolSwap, GammaSwap, both
// capped kinds, corridor, both variance options) was unreachable by every gate
// here. The NaN-poisoning Critical this file now pins lived in exactly that
// blind spot, and VarSwap escaped it only because the memo lane was
// fabricating a 0.0 skew vega at the time. A guard whose fixture cannot reach
// the failing kinds pins nothing, so the fixture is the fix as much as the
// arithmetic is.
[[nodiscard]] DerivPosition deriv_pos_on(DerivKind kind, std::uint32_t uid, double qty,
                                         double T = 0.35) {
  DerivPosition p{};
  p.id = 100u + uid;
  p.uid = uid;
  p.qty = qty;
  p.contract.kind = kind;
  p.contract.maturity_t = T;
  p.contract.notional = 1.0e6;
  p.contract.strike_dec = 0.04;
  p.contract.rv_spec.annualization = 252.0;
  p.contract.rv_spec.n_obs_total = 63u;
  p.contract.rv_spec.n_obs_done = 20u;
  p.contract.rv_spec.rv_done_dec = 0.048;
  if (kind == DerivKind::CappedVarSwap) {
    p.contract.cap_dec = 0.10;
  }
  if (kind == DerivKind::CappedVolSwap) {
    p.contract.cap_dec = 0.40;
  }
  if (kind == DerivKind::CorridorVarSwap) {
    p.contract.corridor_lo = 85.0;
    p.contract.corridor_hi = 120.0;
  }
  if (kind == DerivKind::VolSwap || kind == DerivKind::CappedVolSwap) {
    p.contract.strike_dec = 0.20;  // a VOL strike, not a variance one
  }
  if (kind == DerivKind::VarianceCall) {
    p.contract.strike_dec = 0.03;  // below the fixture's fair variance: live
  }
  if (kind == DerivKind::VariancePut) {
    // ABOVE the fair variance, deliberately. At the call's 0.03 strike this put
    // is worth 0.0022 on a 1e6 notional -- so far out of the money that its
    // whole grid lives at ~1e-3 and every RELATIVE comparison against it is
    // ill-conditioned by construction. A worthless position exercises nothing;
    // this one carries real value and real curvature.
    p.contract.strike_dec = 0.06;
  }
  if (kind == DerivKind::GammaSwap) {
    // An AGED gamma swap needs its own seed spot to rescale the future leg into
    // the accrued leg's units (Lee's w(y) = S_i/S_seed); `price_gamma_swap`
    // refuses the contract without it. That refusal is how the first draft of
    // this sweep found its GammaSwap lane reporting `n_deriv_failed = 1` and an
    // all-zero grid -- passing every finiteness check while exercising nothing.
    p.contract.rv_spec.gamma_seed_spot = kS;
  }
  return p;
}

[[nodiscard]] DerivPosition var_swap_on(std::uint32_t uid, double qty, double T = 0.35) {
  return deriv_pos_on(DerivKind::VarSwap, uid, qty, T);
}

// Every kind `DerivKind` names. Enumerated once so a ninth kind breaks the
// static_assert below rather than silently going untested -- the same drift-pin
// idiom the struct arity checks use, applied to an enum.
constexpr DerivKind kAllDerivKinds[] = {
    DerivKind::VarSwap,       DerivKind::VolSwap,         DerivKind::CappedVarSwap,
    DerivKind::CappedVolSwap, DerivKind::GammaSwap,       DerivKind::CorridorVarSwap,
    DerivKind::VarianceCall,  DerivKind::VariancePut,
};
static_assert(std::size(kAllDerivKinds) == 8u,
              "DerivKind gained a value: add it here so the deriv scenario leg's NaN gates "
              "still cover every kind, then confirm the sweep below still passes");

[[nodiscard]] const char *kind_name(DerivKind k) noexcept {
  switch (k) {
  case DerivKind::VarSwap: return "VarSwap";
  case DerivKind::VolSwap: return "VolSwap";
  case DerivKind::CappedVarSwap: return "CappedVarSwap";
  case DerivKind::CappedVolSwap: return "CappedVolSwap";
  case DerivKind::GammaSwap: return "GammaSwap";
  case DerivKind::CorridorVarSwap: return "CorridorVarSwap";
  case DerivKind::VarianceCall: return "VarianceCall";
  case DerivKind::VariancePut: return "VariancePut";
  }
  return "?";
}

[[nodiscard]] ScenarioGridSpec small_deriv_spec() {
  ScenarioGridSpec s;
  s.spot_pct = {-0.01, 0.0, 0.01};
  s.vol_bump = {-0.005, 0.0, 0.005};
  return s;
}

// The Taylor kernel written out BY HAND, not by calling
// `scenario_deriv_taylor_leg`. That is the point: if the shipped kernel loses a
// term, transposes two sensitivities, or re-associates its sum, this expression
// still says what the expansion is supposed to be.
[[nodiscard]] double hand_deriv_taylor(const DerivGreeks &g, double dS, double dvol, double dt,
                                       double dr, double d_skew, double d_convexity) noexcept {
  return g.delta * dS + 0.5 * g.gamma * dS * dS + g.vega * dvol +
         0.5 * g.volga * dvol * dvol + g.vanna * dS * dvol + g.theta * dt + g.rho * dr +
         g.skew_vega * d_skew + g.convexity_vega * d_convexity + g.charm * dS * dt;
}

} // namespace

// The option leg must not move at all. Asserted on the BITS of every cell and
// on every option-side counter, because "additive" is a claim about numbers,
// not about a diff.
TEST(ScenarioGridDeriv, DerivLegLeavesTheOptionLegByteIdentical) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});
  const std::vector<Position> book = {
      Position{0, OptionContract{1, 100.0, 0.15, Side::Call}, +3.0, 100.0},
      Position{1, OptionContract{1, 95.0, 0.25, Side::Put}, -2.0, 100.0},
  };
  const std::vector<DerivPosition> derivs = {var_swap_on(1u, 2.0)};
  const ScenarioGridSpec spec = small_deriv_spec();

  const auto only_options = scenario_grid(book, ss, spec);
  ASSERT_TRUE(only_options.has_value()) << only_options.error().to_string();
  const auto with_derivs = scenario_grid(book, derivs, ss, spec);
  ASSERT_TRUE(with_derivs.has_value()) << with_derivs.error().to_string();

  ASSERT_EQ(only_options->pnl.size(), with_derivs->pnl.size());
  for (std::size_t c = 0; c < only_options->pnl.size(); ++c) {
    EXPECT_TRUE(bits_equal(only_options->pnl[c], with_derivs->pnl[c])) << "option cell " << c;
    EXPECT_EQ(only_options->route[c], with_derivs->route[c]) << "option route " << c;
  }
  EXPECT_EQ(only_options->n_ok, with_derivs->n_ok);
  EXPECT_EQ(only_options->n_failed, with_derivs->n_failed);
  EXPECT_EQ(only_options->n_exact_fallback_lanes, with_derivs->n_exact_fallback_lanes);

  // And the option-only overload leaves the deriv columns EMPTY rather than
  // zero-filled: "no swap book" and "a swap book worth nothing" are different
  // answers and a reader must be able to tell them apart.
  EXPECT_TRUE(only_options->deriv_pnl.empty());
  EXPECT_TRUE(only_options->deriv_route.empty());
  EXPECT_EQ(only_options->n_deriv_ok, 0u);
  EXPECT_EQ(with_derivs->deriv_pnl.size(), with_derivs->pnl.size());
  EXPECT_EQ(with_derivs->n_deriv_ok, 1u);
  EXPECT_EQ(with_derivs->n_deriv_failed, 0u);
}

// A zero shock is exactly 0.0 on the deriv leg too -- no solve noise leaks into
// the centre cell, which is what makes every other cell readable as a move.
TEST(ScenarioGridDeriv, CenterCellIsExactlyZero) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});
  const std::vector<DerivPosition> derivs = {var_swap_on(1u, 2.0), var_swap_on(1u, -1.5, 0.25)};
  ScenarioGridSpec spec;
  spec.spot_pct = {0.0};
  spec.vol_bump = {0.0};

  const auto r = scenario_grid({}, derivs, ss, spec);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  ASSERT_EQ(r->deriv_pnl.size(), 1u);
  EXPECT_EQ(r->deriv_pnl[0], 0.0);
  EXPECT_EQ(r->deriv_route[0], static_cast<std::uint8_t>(ScenarioRoute::Taylor));
}

// Every Taylor cell equals the hand-written expansion over the same greek
// bundle, term for term and in the same left-to-right order. Routing is
// disabled on both axes so this is a statement about the KERNEL only.
TEST(ScenarioGridDeriv, TaylorCellsMatchTheHandWrittenExpansion) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});
  const std::vector<DerivPosition> derivs = {var_swap_on(1u, 2.0), var_swap_on(1u, -1.5, 0.25)};

  ScenarioGridSpec spec = small_deriv_spec();
  spec.dr = 0.0005;
  spec.dt = 1.0 / 365.25;
  spec.taylor_radius_spot = kInf;
  spec.taylor_radius_vol = kInf;
  ScenarioDerivSpec dspec;
  dspec.d_skew = -0.002;
  dspec.d_convexity = 0.003;

  const auto r = scenario_grid({}, derivs, ss, spec, dspec);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();

  // The reference greek bundle is fetched independently, through the public
  // book pricer, with the same bumps the grid documents it uses.
  DerivGreekBumps bumps{};
  bumps.smile_greeks = true;
  bumps.time_years = spec.dt;
  const auto frame =
      price_deriv_book(ss, std::span<const DerivPosition>{derivs}, DerivConfig{}, true, bumps);
  ASSERT_TRUE(frame.has_value()) << frame.error().to_string();

  const double S = ps.pricing().S;
  for (std::size_t c = 0; c < r->deriv_pnl.size(); ++c) {
    const std::size_t i = c / spec.vol_bump.size();
    const std::size_t j = c % spec.vol_bump.size();
    const double dS = spec.spot_pct[i] * S;
    double expected = 0.0;
    for (const auto &row : frame->rows) {
      expected += hand_deriv_taylor(row.greeks, dS, spec.vol_bump[j], spec.dt, spec.dr,
                                    dspec.d_skew, dspec.d_convexity);
    }
    EXPECT_EQ(r->deriv_route[c], static_cast<std::uint8_t>(ScenarioRoute::Taylor));
    EXPECT_NEAR(r->deriv_pnl[c], expected, 1.0e-9 * std::abs(expected) + 1.0e-9) << "cell " << c;
  }
}

// A pure smile shock with no spot or vol move must land entirely on the two
// smile terms. Zero spot and zero vol shocks kill every other term exactly, so
// this isolates the sensitivities the option leg has no counterpart for.
TEST(ScenarioGridDeriv, SmileShockLandsOnTheSmileTermsAlone) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});
  const std::vector<DerivPosition> derivs = {var_swap_on(1u, 2.0)};

  ScenarioGridSpec spec;
  spec.spot_pct = {0.0};
  spec.vol_bump = {0.0};
  spec.taylor_radius_spot = kInf;
  spec.taylor_radius_vol = kInf;
  ScenarioDerivSpec dspec;
  dspec.d_skew = -0.002;

  const auto r = scenario_grid({}, derivs, ss, spec, dspec);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();

  DerivGreekBumps bumps{};
  bumps.smile_greeks = true;
  const auto frame =
      price_deriv_book(ss, std::span<const DerivPosition>{derivs}, DerivConfig{}, true, bumps);
  ASSERT_TRUE(frame.has_value()) << frame.error().to_string();
  const double skew_vega = frame->rows[0].greeks.skew_vega;
  ASSERT_TRUE(std::isfinite(skew_vega));
  ASSERT_NE(skew_vega, 0.0);

  EXPECT_NEAR(r->deriv_pnl[0], skew_vega * dspec.d_skew,
              1.0e-9 * std::abs(skew_vega * dspec.d_skew));
  // A long var swap loses when the skew flattens: skew_vega < 0 (pinned on
  // DerivGreeks::skew_vega), so a negative d_skew is a gain.
  EXPECT_GT(r->deriv_pnl[0], 0.0);
}

// THE ACCEPTANCE CLAIM. A Taylor cell and a full reprice must be two views of
// one model. Both grids are built over the same book and the same shocks; the
// only difference is the routing radii.
//
// The bound is not a wished-for constant. The Taylor remainder is O(h^3) in the
// shock, so the test HALVES every shock and requires the disagreement to fall
// by at least 4x -- a first-order-wrong kernel would hold its error roughly
// constant, and a second-order-wrong one would fall by only 2x. The absolute
// relative bound is the weaker companion check.
TEST(ScenarioGridDeriv, TaylorAndExactAgreeToTheirOwnRemainder) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});
  const std::vector<DerivPosition> derivs = {var_swap_on(1u, 2.0)};

  const auto worst_gap = [&](double scale) {
    ScenarioGridSpec taylor_spec;
    taylor_spec.spot_pct = {-0.01 * scale, 0.0, 0.01 * scale};
    taylor_spec.vol_bump = {-0.005 * scale, 0.0, 0.005 * scale};
    taylor_spec.taylor_radius_spot = kInf; // never route
    taylor_spec.taylor_radius_vol = kInf;

    ScenarioGridSpec exact_spec = taylor_spec;
    exact_spec.taylor_radius_spot = -1.0; // |x| > -1 is always true: always route
    exact_spec.taylor_radius_vol = -1.0;

    const auto t = scenario_grid({}, derivs, ss, taylor_spec);
    const auto e = scenario_grid({}, derivs, ss, exact_spec);
    EXPECT_TRUE(t.has_value());
    EXPECT_TRUE(e.has_value());
    double gap = 0.0;
    double mag = 0.0;
    for (std::size_t c = 0; c < t->deriv_pnl.size(); ++c) {
      EXPECT_EQ(e->deriv_route[c], static_cast<std::uint8_t>(ScenarioRoute::Exact));
      gap = std::max(gap, std::abs(t->deriv_pnl[c] - e->deriv_pnl[c]));
      mag = std::max(mag, std::abs(e->deriv_pnl[c]));
    }
    EXPECT_EQ(e->n_deriv_exact_fallback_lanes, 0u) << "every shocked reprice must have solved";
    return std::pair<double, double>{gap, mag};
  };

  const auto [gap_h, mag_h] = worst_gap(1.0);
  const auto [gap_half, mag_half] = worst_gap(0.5);
  ASSERT_GT(mag_h, 0.0);
  ASSERT_GT(gap_h, 0.0) << "an exactly-zero gap would mean the Exact route never repriced";

  EXPECT_LT(gap_h, 0.02 * mag_h) << "gap=" << gap_h << " cell magnitude=" << mag_h;
  EXPECT_LT(gap_half, gap_h / 4.0)
      << "halving the shock must shrink the Taylor remainder super-quadratically: " << gap_h
      << " -> " << gap_half << " (mag " << mag_h << " -> " << mag_half << ")";
}

// A deriv position whose uid has no surface is excluded from every cell and
// counted once -- the deriv mirror of FailedContractExcludedEverywhere. No NaN
// reaches a cell total.
TEST(ScenarioGridDeriv, FailedDerivPositionIsExcludedEverywhere) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});
  const std::vector<DerivPosition> good = {var_swap_on(1u, 2.0)};
  std::vector<DerivPosition> mixed = good;
  mixed.push_back(var_swap_on(9u, 5.0)); // uid 9 is not registered

  ScenarioGridSpec spec = small_deriv_spec();
  spec.taylor_radius_spot = kInf;
  spec.taylor_radius_vol = kInf;

  const auto only_good = scenario_grid({}, good, ss, spec);
  ASSERT_TRUE(only_good.has_value()) << only_good.error().to_string();
  const auto with_bad = scenario_grid({}, mixed, ss, spec);
  ASSERT_TRUE(with_bad.has_value()) << with_bad.error().to_string();

  EXPECT_EQ(with_bad->n_deriv_ok, 1u);
  EXPECT_EQ(with_bad->n_deriv_failed, 1u);
  for (std::size_t c = 0; c < only_good->deriv_pnl.size(); ++c) {
    EXPECT_TRUE(std::isfinite(with_bad->deriv_pnl[c]));
    EXPECT_TRUE(bits_equal(only_good->deriv_pnl[c], with_bad->deriv_pnl[c])) << "cell " << c;
  }
}

// ── The NaN gates (Task F-8 review round 1, Critical) ───────────────────────
//
// `DerivGreeks` carries "not computed" as NaN in SIX of the ten slots the
// Taylor kernel reads, on documented conditions the DEFAULT configuration hits.
// `NaN * 0.0` is NaN, so an unguarded product poisons all ten terms. Round 1 of
// this leg shipped exactly that: a VolSwap under the documented default
// `ScenarioDerivSpec{}` returned nine NaN cells while reporting
// `n_deriv_ok = 1, n_deriv_failed = 0` -- success, and nothing but NaN.
//
// The three tests below are the gate. The first is the kernel's own rule,
// checked slot by slot with a deliberately-poisoned `DerivGreeks`; the second
// sweeps EVERY kind through the real grid under the real default; the third
// covers the case a zero shock cannot rescue -- a shock on an axis whose
// sensitivity genuinely was not computed.

// Each of the six NaN-capable slots, one at a time, with its own shock zero.
// Every other term must survive: a guard that returned 0.0 for the whole cell
// would pass a naive "is it finite" check while destroying the answer, so the
// non-poisoned terms are asserted against their exact hand-written products.
TEST(ScenarioGridDeriv, EveryNaNCapableTermIsGatedByItsOwnShock) {
  const double nan = std::numeric_limits<double>::quiet_NaN();

  DerivGreeks g{};
  g.delta = 3.0;
  g.gamma = 5.0;
  g.vega = 7.0;
  g.volga = 11.0;
  g.vanna = 13.0;
  g.theta = 17.0;
  g.rho = 19.0;
  g.charm = 23.0;
  g.skew_vega = 29.0;
  g.convexity_vega = 31.0;

  // Baseline: everything finite, every shock live.
  const double dS = 2.0, dvol = 0.5, dt = 0.25, dr = 0.125, dsk = 0.0625, dcv = 0.03125;
  const double full = scenario_deriv_taylor_leg(g, dS, dvol, dt, dr, dsk, dcv);
  ASSERT_TRUE(std::isfinite(full));

  // theta NaN (a contract shorter than the roll) + dt == 0.
  {
    DerivGreeks p = g;
    p.theta = nan;
    p.charm = nan;
    const double v = scenario_deriv_taylor_leg(p, dS, dvol, 0.0, dr, dsk, dcv);
    EXPECT_TRUE(std::isfinite(v)) << "theta/charm NaN with dt == 0 must not poison the cell";
    EXPECT_DOUBLE_EQ(v, scenario_deriv_taylor_leg(g, dS, dvol, 0.0, dr, dsk, dcv));
  }
  // vanna/charm NaN (second_order off) + dvol == 0 kills vanna, dt == 0 kills charm.
  {
    DerivGreeks p = g;
    p.vanna = nan;
    p.charm = nan;
    const double v = scenario_deriv_taylor_leg(p, dS, 0.0, 0.0, dr, dsk, dcv);
    EXPECT_TRUE(std::isfinite(v)) << "vanna/charm NaN with their shocks 0 must not poison";
    EXPECT_DOUBLE_EQ(v, scenario_deriv_taylor_leg(g, dS, 0.0, 0.0, dr, dsk, dcv));
  }
  // The two smile vegas (smile_greeks off -- the DEFAULT) + zero smile shocks.
  {
    DerivGreeks p = g;
    p.skew_vega = nan;
    p.convexity_vega = nan;
    const double v = scenario_deriv_taylor_leg(p, dS, dvol, dt, dr, 0.0, 0.0);
    EXPECT_TRUE(std::isfinite(v)) << "smile vegas NaN with no smile shock must not poison";
    EXPECT_DOUBLE_EQ(v, scenario_deriv_taylor_leg(g, dS, dvol, dt, dr, 0.0, 0.0));
  }
  // ALL SIX at once, every corresponding shock zero -- the real default grid.
  {
    DerivGreeks p = g;
    p.theta = nan;
    p.charm = nan;
    p.vanna = nan;
    p.skew_vega = nan;
    p.convexity_vega = nan;
    const double v = scenario_deriv_taylor_leg(p, dS, 0.0, 0.0, dr, 0.0, 0.0);
    EXPECT_TRUE(std::isfinite(v));
    // Only the two surviving terms, hand-written.
    EXPECT_DOUBLE_EQ(v, g.delta * dS + 0.5 * g.gamma * dS * dS + g.rho * dr);
  }
  // And the honest converse: a NON-ZERO shock against a NaN sensitivity still
  // propagates. There is no answer to give, and manufacturing a 0.0 would read
  // as "measured, no exposure".
  {
    DerivGreeks p = g;
    p.skew_vega = nan;
    EXPECT_TRUE(std::isnan(scenario_deriv_taylor_leg(p, dS, dvol, dt, dr, dsk, dcv)));
    DerivGreeks q = g;
    q.theta = nan;
    EXPECT_TRUE(std::isnan(scenario_deriv_taylor_leg(q, dS, dvol, dt, dr, dsk, dcv)));
  }
  // Gating is bit-identical to the bare product whenever the sensitivity is
  // finite -- the guard must not have changed any number that was already right.
  EXPECT_TRUE(bits_equal(full, g.delta * dS + 0.5 * g.gamma * dS * dS + g.vega * dvol +
                                   0.5 * g.volga * dvol * dvol + g.vanna * dS * dvol +
                                   g.theta * dt + g.rho * dr + g.charm * dS * dt +
                                   g.skew_vega * dsk + g.convexity_vega * dcv));
}

// THE REGRESSION THIS ROUND EXISTS FOR. Every kind, through the real grid,
// under the documented default `ScenarioDerivSpec{}` -- the configuration that
// returned nine NaN cells stamped Ok.
//
// VarSwap is in the sweep but is not what makes it a gate: `price_deriv_book`
// memoizes VarSwap rows only, so VarSwap took a different path from the other
// seven kinds and was the one kind round 1's fixture could see.
TEST(ScenarioGridDeriv, EveryKindProducesFiniteCellsUnderTheDefaultSpec) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});
  const ScenarioGridSpec spec = small_deriv_spec();

  for (const DerivKind kind : kAllDerivKinds) {
    const std::vector<DerivPosition> derivs = {deriv_pos_on(kind, 1u, 2.0)};
    const auto r = scenario_grid({}, derivs, ss, spec);  // DEFAULT ScenarioDerivSpec
    ASSERT_TRUE(r.has_value()) << kind_name(kind) << ": " << r.error().to_string();
    ASSERT_EQ(r->deriv_pnl.size(), 9u) << kind_name(kind);

    // The accounting identity: every position lands in exactly one bucket.
    EXPECT_EQ(r->n_deriv_ok + r->n_deriv_failed + r->n_deriv_missing_sensitivity, derivs.size())
        << kind_name(kind);

    for (std::size_t c = 0; c < r->deriv_pnl.size(); ++c) {
      EXPECT_TRUE(std::isfinite(r->deriv_pnl[c]))
          << kind_name(kind) << " cell " << c << " = " << r->deriv_pnl[c];
    }
    // A kind that priced must actually MOVE somewhere on the grid. An
    // all-finite matrix of zeros would pass the check above while saying
    // nothing, and is what excluding the position would produce.
    if (r->n_deriv_ok > 0u) {
      bool any_nonzero = false;
      for (const double v : r->deriv_pnl) {
        any_nonzero = any_nonzero || (v != 0.0);
      }
      EXPECT_TRUE(any_nonzero) << kind_name(kind) << " contributed nothing to any cell";
    } else {
      // If it did NOT price, say which bucket -- silence here is what let the
      // Critical through.
      EXPECT_GT(r->n_deriv_failed + r->n_deriv_missing_sensitivity, 0u) << kind_name(kind);
    }
  }
}

// THE CASE THAT WAS STILL LIVE AT HEAD, and the narrowest statement of why the
// guard had to be generalised rather than extended by one more special case.
//
// A contract SHORTER than `DerivGreekBumps::time_years` (default 1/365.25) gets
// theta AND charm NaN. Task F-7 round 1 guarded the two smile terms, which
// closed the door the reviewer came through; this one stayed open. Measured
// out-of-tree against 39c934c with the reviewed `scenario_grid.hpp`/`.cpp`
// linked ahead of the library: 9/9 NaN cells with `n_deriv_ok = 1`, on a grid
// whose `dt` is ZERO -- the caller asked for no time roll at all and got a
// matrix of NaN stamped Ok.
//
// The position must come back INCLUDED, not excluded: with `dt == 0` no theta
// is ever read, so nothing is missing and contributing the position in full is
// the correct answer rather than a lenient one.
TEST(ScenarioGridDeriv, AShortContractDoesNotPoisonAGridThatAsksForNoTimeRoll) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});

  ScenarioGridSpec spec = small_deriv_spec();
  ASSERT_EQ(spec.dt, 0.0) << "the point of this test is a grid with no time roll";

  for (const DerivKind kind : kAllDerivKinds) {
    DerivPosition p = deriv_pos_on(kind, 1u, 2.0);
    p.contract.maturity_t = 0.001;  // ~0.37 days, under the default roll
    const std::vector<DerivPosition> derivs = {p};

    const auto r = scenario_grid({}, derivs, ss, spec);
    ASSERT_TRUE(r.has_value()) << kind_name(kind) << ": " << r.error().to_string();
    for (std::size_t c = 0; c < r->deriv_pnl.size(); ++c) {
      EXPECT_TRUE(std::isfinite(r->deriv_pnl[c]))
          << kind_name(kind) << " cell " << c << " = " << r->deriv_pnl[c];
    }
    EXPECT_EQ(r->n_deriv_ok + r->n_deriv_failed + r->n_deriv_missing_sensitivity, 1u)
        << kind_name(kind);
    // A NaN theta must not cost the position its place in a grid that never
    // reads theta.
    if (r->n_deriv_failed == 0u) {
      EXPECT_EQ(r->n_deriv_missing_sensitivity, 0u)
          << kind_name(kind) << ": excluded over a sensitivity this grid never reads";
    }
  }
}

// The case a zero shock cannot rescue: a smile shock against a book whose
// greeks were solved WITHOUT smile_greeks is impossible by construction (the
// grid turns the flag on when the shock is set), but a `dt` roll against a
// contract SHORTER than the roll is not -- `theta` and `charm` come back NaN
// and no flag can fix it. That position is excluded from every cell and counted
// in its own bucket, never contributed as a 0.0.
TEST(ScenarioGridDeriv, AContractTooShortToRollIsExcludedAndCountedNotZeroed) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});

  ScenarioGridSpec spec = small_deriv_spec();
  spec.dt = 0.02;  // ~7 days
  spec.taylor_radius_spot = kInf;
  spec.taylor_radius_vol = kInf;

  const DerivPosition rollable = deriv_pos_on(DerivKind::VolSwap, 1u, 2.0, 0.35);
  const DerivPosition too_short = deriv_pos_on(DerivKind::VolSwap, 1u, 3.0, 0.01);

  const auto solo = scenario_grid({}, {rollable}, ss, spec);
  ASSERT_TRUE(solo.has_value()) << solo.error().to_string();
  ASSERT_EQ(solo->n_deriv_ok, 1u);

  const auto mixed = scenario_grid({}, {rollable, too_short}, ss, spec);
  ASSERT_TRUE(mixed.has_value()) << mixed.error().to_string();

  EXPECT_EQ(mixed->n_deriv_ok, 1u);
  EXPECT_EQ(mixed->n_deriv_ok + mixed->n_deriv_failed + mixed->n_deriv_missing_sensitivity, 2u);
  // NAMED, not disjunctive. Round 1 asserted `missing + failed >= 1`, which
  // passes whichever bucket fills -- so the distinction the second Tier-A field
  // exists to carry ("a failed solve and an unmeasured axis have different
  // fixes") was unpinned, and moving the sufficiency check to before the
  // `pos_ok` assignment would have counted every exclusion as `failed` with the
  // whole suite still green. This lot PRICED; its theta is NaN because the
  // contract is shorter than the roll. That is the missing-sensitivity bucket
  // and nothing else.
  EXPECT_EQ(mixed->n_deriv_missing_sensitivity, 1u)
      << "a lot that priced but cannot be rolled belongs in the missing-sensitivity "
         "bucket, not in n_deriv_failed";
  EXPECT_EQ(mixed->n_deriv_failed, 0u) << "the base greek solve succeeded for both lots";

  // Excluded means excluded: the surviving book prices exactly as if the
  // unshockable position were absent, on the bits, with no NaN anywhere.
  for (std::size_t c = 0; c < solo->deriv_pnl.size(); ++c) {
    EXPECT_TRUE(std::isfinite(mixed->deriv_pnl[c])) << "cell " << c;
    EXPECT_TRUE(bits_equal(solo->deriv_pnl[c], mixed->deriv_pnl[c])) << "cell " << c;
  }
}

// The smile terms must work on a NON-memoised kind too. Round 1 checked
// `skew_vega` only on a VarSwap, which is the one kind that reaches
// `price_deriv_book`'s shared block -- and F-7 round 1 found that block serving
// a fabricated 0.0 there. A VolSwap takes the unmemoised path, so this is the
// same claim measured through different code.
TEST(ScenarioGridDeriv, SmileShockLandsOnTheSmileTermsForANonMemoisedKind) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});
  const std::vector<DerivPosition> derivs = {deriv_pos_on(DerivKind::VolSwap, 1u, 2.0)};

  ScenarioGridSpec spec;
  spec.spot_pct = {0.0};
  spec.vol_bump = {0.0};
  spec.taylor_radius_spot = kInf;
  spec.taylor_radius_vol = kInf;
  ScenarioDerivSpec dspec;
  dspec.d_skew = -0.002;

  const auto r = scenario_grid({}, derivs, ss, spec, dspec);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  ASSERT_EQ(r->n_deriv_ok, 1u) << "a VolSwap must be shockable on the smile axis";

  DerivGreekBumps bumps{};
  bumps.smile_greeks = true;
  const auto frame =
      price_deriv_book(ss, std::span<const DerivPosition>{derivs}, DerivConfig{}, true, bumps);
  ASSERT_TRUE(frame.has_value()) << frame.error().to_string();
  const double skew_vega = frame->rows[0].greeks.skew_vega;
  ASSERT_TRUE(std::isfinite(skew_vega)) << "smile_greeks must produce a finite skew vega here";
  ASSERT_NE(skew_vega, 0.0);

  EXPECT_NEAR(r->deriv_pnl[0], skew_vega * dspec.d_skew,
              1.0e-9 * std::abs(skew_vega * dspec.d_skew));
}

// The Taylor-vs-Exact agreement claim, re-run on the non-memoised kinds. The
// acceptance test above uses a VarSwap; the O(h^3) convergence argument is a
// statement about the EXPANSION, so it has to hold for every kind that has one.
TEST(ScenarioGridDeriv, TaylorAndExactAgreeAcrossTheKindSpace) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});

  for (const DerivKind kind : kAllDerivKinds) {
    const std::vector<DerivPosition> derivs = {deriv_pos_on(kind, 1u, 2.0)};

    const auto gap_at = [&](double scale) {
      ScenarioGridSpec t_spec;
      t_spec.spot_pct = {-0.01 * scale, 0.0, 0.01 * scale};
      t_spec.vol_bump = {-0.005 * scale, 0.0, 0.005 * scale};
      t_spec.taylor_radius_spot = kInf;
      t_spec.taylor_radius_vol = kInf;
      ScenarioGridSpec e_spec = t_spec;
      e_spec.taylor_radius_spot = -1.0;
      e_spec.taylor_radius_vol = -1.0;

      const auto t = scenario_grid({}, derivs, ss, t_spec);
      const auto e = scenario_grid({}, derivs, ss, e_spec);
      EXPECT_TRUE(t.has_value()) << kind_name(kind);
      EXPECT_TRUE(e.has_value()) << kind_name(kind);
      double gap = 0.0;
      double mag = 0.0;
      for (std::size_t c = 0; c < t->deriv_pnl.size(); ++c) {
        gap = std::max(gap, std::abs(t->deriv_pnl[c] - e->deriv_pnl[c]));
        mag = std::max(mag, std::abs(e->deriv_pnl[c]));
      }
      return std::pair<double, double>{gap, mag};
    };

    const auto [gap_h, mag_h] = gap_at(1.0);
    const auto [gap_half, mag_half] = gap_at(0.5);
    (void)mag_half;
    ASSERT_GT(mag_h, 0.0) << kind_name(kind) << ": the Exact route produced a flat grid";
    // MEASURED after the centre-pin fix: the worst kind is CorridorVarSwap at
    // 0.105% of its cell, and every kind's ratio is 8.02-8.40. Both bounds sit
    // an order of magnitude off what holds, so this is a regression guard on
    // the expansion staying third-order, not a pin on a number. Before the pin,
    // VolSwap sat at 0.99% with a ratio of 2.008 -- inside a naive relative
    // bound, and caught only by the ratio.
    EXPECT_LT(gap_h, 0.005 * mag_h)
        << kind_name(kind) << ": gap=" << gap_h << " magnitude=" << mag_h;
    EXPECT_LT(gap_half, gap_h / 4.0 + 1.0e-12)
        << kind_name(kind) << ": remainder did not shrink super-quadratically, " << gap_h
        << " -> " << gap_half;
  }
}

// ── The SHOCK-GATING agreement, and precisely what it does not see ──────────
//
// `scenario_deriv_taylor_leg` and `scenario_deriv_greeks_sufficient` are two
// hand-written copies of one rule, 25 lines apart. This test runs them against
// each other over all 64 on/off combinations of the six shocks, with an ALL-NaN
// `DerivGreeks`: the kernel is then finite iff it read NOTHING and the predicate
// is true iff it demands NOTHING, so `isfinite(kernel) == sufficient` compares
// the two gate-sets directly, in both directions, over every branch of both.
//
// WHAT THIS CATCHES:
//   * a term gated in the kernel but missing from the predicate, WHEN the field
//     it reads is one the predicate would otherwise have refused on -- deleting
//     the `has_dt -> theta` gate gives exactly 1 mismatch of 64, at mask 4.
//   * a term in the predicate but not in the kernel; the equality fails the
//     other way.
//   * an UNGATED term on any `double` member of `DerivGreeks` -- the kernel then
//     reads at mask 0, where the predicate must say "demands nothing". This is
//     true only because the fixture below sets all THIRTEEN doubles to NaN; with
//     the ten the kernel reads it was false for the three it does not, and
//     measurably so (ungated-`pv`: 0/64). `quote`'s nested doubles are still
//     outside it, for the same reason `kNaNSlots` cannot hold them.
//   * a term needing a NEW shock field -> `ScenarioDerivSpec`'s arity pin.
//   * a term needing a NEW `DerivGreeks` field -> the static_assert below.
//
// WHAT THIS CANNOT CATCH, AND THE REASON, BECAUSE THE ROUND THAT WROTE THIS TEST
// CLAIMED OTHERWISE:
//
// The equality is SYMMETRIC and FIELD-BLIND. It compares "did the kernel touch
// anything NaN" against "does the predicate demand anything" -- two booleans,
// not two field sets. So it is blind to a term that reads a field the predicate
// never checks, under a shock that already forces the predicate false for a
// DIFFERENT field. Concretely:
//
//     const double pcarry = (dt == 0.0) ? 0.0 : g.theta_carry * dt;
//
// gated, unmirrored, on a field `scenario_deriv_greeks_sufficient` never names.
// Measured: this mutant scores 0/64 here. It passes this test completely.
// `ASufficientVerdictAlwaysYieldsAFiniteKernel` below is the artifact that
// catches it, and its `kNaNSlots` list -- not this equality -- is what stands
// between this codebase and that shape.
//
// This test's job is the gate-set agreement, which it does well and cheaply. It
// is not the exhaustiveness proof the round that added it called it.
TEST(ScenarioGridDeriv, TheKernelAndItsSufficiencyPredicateAgreeOnAll64ShockCombinations) {
  // If `DerivGreeks` grows a field, decide whether the deriv Taylor kernel reads
  // it. This assert is the only thing that asks.
  static_assert(atx::vol::detail::aggregate_arity_is_v<DerivGreeks, 14>,
                "DerivGreeks gained a field: decide whether scenario_deriv_taylor_leg reads it, "
                "gate it in scenario_deriv_greeks_sufficient if so, add it to kNaNSlots below "
                "(which must name EVERY double member, read or not), then update this pin.");

  const double nan = std::numeric_limits<double>::quiet_NaN();
  DerivGreeks all_nan{};
  all_nan.delta = nan;
  all_nan.gamma = nan;
  all_nan.vega = nan;
  all_nan.volga = nan;
  all_nan.vanna = nan;
  all_nan.theta = nan;
  all_nan.rho = nan;
  all_nan.charm = nan;
  all_nan.skew_vega = nan;
  all_nan.convexity_vega = nan;
  // EVERY double member, not just the ten the kernel reads -- the same
  // correction `kNaNSlots` needed, applied here in fix round 5 after it was
  // condemned ninety lines below and left standing here.
  //
  // With only the ten set, this harness inherited its reach from `DerivGreeks`'
  // DEFAULTS: `theta_carry`/`theta_zero_fixing` are `kQuietNaN`-defaulted so an
  // ungated term on them was caught, while `pv` defaults to `0.0` so an ungated
  // term on IT was not. MEASURED: ungated-`pv` scored 0/64 (missed entirely),
  // ungated-`theta_carry` scored 1/64 at mask 0. Setting all thirteen takes
  // ungated-`pv` to 1/64 at mask 0 and leaves the shipped kernel at 0/64.
  all_nan.pv = nan;
  all_nan.theta_carry = nan;
  all_nan.theta_zero_fixing = nan;

  // Deliberately NOT all 1.0: a shock of 1.0 on every axis would let a missing
  // multiplication hide. These are distinct, none is a power of two, and none
  // divides another.
  constexpr double kOn[6] = {2.0, 0.5, 0.25, 0.125, 0.0625, 0.03125};

  std::size_t agreed = 0;
  std::size_t sufficient_count = 0;
  for (unsigned mask = 0; mask < 64u; ++mask) {
    const double dS = (mask & 1u) ? kOn[0] : 0.0;
    const double dvol = (mask & 2u) ? kOn[1] : 0.0;
    const double dt = (mask & 4u) ? kOn[2] : 0.0;
    const double dr = (mask & 8u) ? kOn[3] : 0.0;
    ScenarioDerivSpec dspec;
    dspec.d_skew = (mask & 16u) ? kOn[4] : 0.0;
    dspec.d_convexity = (mask & 32u) ? kOn[5] : 0.0;

    const double v =
        scenario_deriv_taylor_leg(all_nan, dS, dvol, dt, dr, dspec.d_skew, dspec.d_convexity);
    const bool kernel_reads_nothing = std::isfinite(v);
    const bool demands_nothing = scenario_deriv_greeks_sufficient(
        all_nan, dS != 0.0, dvol != 0.0, dt != 0.0, dr != 0.0, dspec);

    EXPECT_EQ(kernel_reads_nothing, demands_nothing)
        << "shock mask " << mask << " (dS=" << dS << " dvol=" << dvol << " dt=" << dt
        << " dr=" << dr << " d_skew=" << dspec.d_skew << " d_convexity=" << dspec.d_convexity
        << "): the Taylor kernel " << (kernel_reads_nothing ? "read nothing" : "read a NaN slot")
        << " but the sufficiency predicate says it "
        << (demands_nothing ? "needs nothing" : "needs a slot") << ". The two are hand-written "
        << "copies of one rule; one of them was changed without the other.";
    agreed += static_cast<std::size_t>(kernel_reads_nothing == demands_nothing);
    sufficient_count += static_cast<std::size_t>(demands_nothing);
  }
  EXPECT_EQ(agreed, 64u);
  // Non-vacuity: exactly ONE combination demands nothing (all shocks zero). If a
  // future edit made the predicate trivially true or trivially false, the
  // equality above could still pass while checking nothing.
  EXPECT_EQ(sufficient_count, 1u)
      << "only the all-zero shock should need no sensitivity; a predicate that is "
         "constant makes the agreement check vacuous";
}

// THIS IS THE TEST THAT CLOSES THE GAP. The equality above is symmetric and
// field-blind; this one is neither, and it is the direction that ships: a
// predicate saying "sufficient" while the kernel reads a NaN puts a NaN in a
// cell total with the grid reporting success.
//
// ── `kNaNSlots` MUST NAME EVERY `double` MEMBER OF `DerivGreeks` ───────────
//
// Not every slot the kernel reads TODAY. That distinction is the whole test.
// A list of what the kernel currently reads is definitionally unable to catch a
// kernel that STARTS reading something it did not before, which is exactly the
// shape the sibling test cannot see either:
//
//     const double pcarry = (dt == 0.0) ? 0.0 : g.theta_carry * dt;
//
// gated, unmirrored, on a field the predicate never names. `theta_carry` and
// `theta_zero_fixing` are `kQuietNaN`-DEFAULTED (derivatives.hpp), so before
// this round they were left at NaN by the `DerivGreeks g{}` below and this test
// caught that mutant BY ACCIDENT -- the strength came from a field initializer
// in another header that no test named, and it evaporated if anyone ever
// defaulted them to 0.0. It is now explicit: they are listed, set to 1.0 with
// everything else, and NaN'd deliberately in their turn.
//
// So an omission from this list is a SILENT HOLE, not a weakening. The previous
// comment here said "incompleteness only weakens the test"; that was false, and
// it was false about the one list doing the real work.
//
// Enumerated against the struct rather than pattern-matched, because a fence
// expressed as a name pattern has exactly the blind spot the pattern has. All 14
// members of `DerivGreeks`: 13 doubles, all listed below; plus `quote`, a nested
// `DerivQuote` this table's `double DerivGreeks::*` type CANNOT express -- the
// kernel does not read it, and a future term reading `g.quote.anything` needs
// this table's type widened, which is stated here because nothing else would say
// it. The arity pin in the sibling test is what notices a 15th member.
TEST(ScenarioGridDeriv, ASufficientVerdictAlwaysYieldsAFiniteKernel) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const struct {
    const char *name;
    double DerivGreeks::*slot;
  } kNaNSlots[] = {
      // Read by the kernel today, and gated in the predicate.
      {"delta", &DerivGreeks::delta},   {"gamma", &DerivGreeks::gamma},
      {"vega", &DerivGreeks::vega},     {"volga", &DerivGreeks::volga},
      {"vanna", &DerivGreeks::vanna},   {"theta", &DerivGreeks::theta},
      {"rho", &DerivGreeks::rho},       {"charm", &DerivGreeks::charm},
      {"skew_vega", &DerivGreeks::skew_vega},
      {"convexity_vega", &DerivGreeks::convexity_vega},
      // NOT read by the kernel today, and therefore not gated -- which is
      // precisely why they belong here. These three are where a gated-but-
      // unmirrored eleventh term would land, and listing them is what turns
      // catching it from an accident into a property.
      {"theta_carry", &DerivGreeks::theta_carry},
      {"theta_zero_fixing", &DerivGreeks::theta_zero_fixing},
      {"pv", &DerivGreeks::pv},
  };
  static_assert(std::size(kNaNSlots) == 13u,
                "kNaNSlots must name every double member of DerivGreeks -- 13 of its 14 "
                "members; the 14th, `quote`, is a nested aggregate this table cannot hold");

  // ── POSITIVE CONTROL, MEASURED ────────────────────────────────────────────
  //
  // Non-vacuity demonstrated, not asserted -- it was accidental until this round
  // and nothing recorded that. Compiled out-of-tree against a copy of
  // scenario_grid.hpp carrying the gated-but-unmirrored term
  // `pcarry = (dt == 0.0) ? 0.0 : g.theta_carry * dt`:
  //
  //     shipped :  sibling 0 leaks       2^6 equality 0/64
  //     mutant  :  sibling 32 leaks      2^6 equality 0/64   <- the equality MISSES it
  //                first leak: slot theta_carry, mask 4
  //
  // 32 is exactly the number of shock masks with `dt` set (2^5), all on
  // `theta_carry`'s own pass -- so the catch is attributable to one listed slot
  // rather than smeared across the table. That attributability IS the fix: an
  // earlier review measured 152 leaks for the same mutant because
  // `theta_carry`/`theta_zero_fixing`/`pv` were absent from the list and sat at
  // their kQuietNaN defaults, poisoning every OTHER slot's pass as a side
  // effect. Same mutant caught, but for a reason no test named and that a
  // one-word change to a defaulted initializer in another header would have
  // silently removed.

  for (const auto &slot : kNaNSlots) {
    DerivGreeks g{};
    g.delta = 1.0;
    g.gamma = 1.0;
    g.vega = 1.0;
    g.volga = 1.0;
    g.vanna = 1.0;
    g.theta = 1.0;
    g.rho = 1.0;
    g.charm = 1.0;
    g.skew_vega = 1.0;
    g.convexity_vega = 1.0;
    // The three the kernel does not read are set FINITE here too, so this test
    // no longer inherits its strength from `DerivGreeks`' kQuietNaN defaults.
    // Each is NaN'd only on its own pass through `kNaNSlots`.
    g.theta_carry = 1.0;
    g.theta_zero_fixing = 1.0;
    g.pv = 1.0;
    g.*(slot.slot) = nan;

    for (unsigned mask = 0; mask < 64u; ++mask) {
      const double dS = (mask & 1u) ? 2.0 : 0.0;
      const double dvol = (mask & 2u) ? 0.5 : 0.0;
      const double dt = (mask & 4u) ? 0.25 : 0.0;
      const double dr = (mask & 8u) ? 0.125 : 0.0;
      ScenarioDerivSpec dspec;
      dspec.d_skew = (mask & 16u) ? 0.0625 : 0.0;
      dspec.d_convexity = (mask & 32u) ? 0.03125 : 0.0;

      if (!scenario_deriv_greeks_sufficient(g, dS != 0.0, dvol != 0.0, dt != 0.0, dr != 0.0,
                                            dspec)) {
        continue; // correctly refused; the grid excludes this position
      }
      const double v =
          scenario_deriv_taylor_leg(g, dS, dvol, dt, dr, dspec.d_skew, dspec.d_convexity);
      EXPECT_TRUE(std::isfinite(v))
          << slot.name << " is NaN and the predicate admitted shock mask " << mask
          << ", but the kernel read it. That is a NaN in a cell total with the grid "
             "reporting success -- the exact failure the predicate exists to prevent.";
    }
  }
}

// ── I-7 / the axis-narrow fixture: dt, dr, and the centre-resolving default ──
//
// Round 1's two convergence sweeps both ran with `dr = 0`, `dt = 0` and a
// default `ScenarioDerivSpec{}`, so the Exact deriv route was compared against
// Taylor on the two cheapest axes only. `DerivShock::time_roll`,
// `DerivShock::rate_shift`, the `pv *= exp(-dr*T')` rescale, the
// "time_roll consumes the whole tenor" guard, and the entire
// `centre == nullptr` default branch were executed by NOTHING in the tree --
// while the centre-scheme pin whose 776x win round 1 reported applies to those
// axes too, and `dt` is the axis where the shocked reprice changes the
// contract's own tenor and the pin therefore does the most work.
//
// That is the same shape as round 0 (kind-narrow) and round 1 (axis-narrow),
// one dimension over, which is why these tests exercise the axes rather than
// the entry point.

// The rate shock's exact-rescale claim, checked rather than asserted. The header
// says `pv * exp(-dr*T')` IS the reprice under a parallel shift, because every
// kind here is a discounted expectation whose undiscounted leg never sees the
// discount curve (`rho = -T*pv` analytically, on all four assembly paths). So a
// pure rate shock must move the mark by EXACTLY that factor -- not to a
// tolerance, to the ratio.
TEST(ScenarioGridDeriv, ARateShockIsExactlyADiscountRescale) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});
  const SurfaceRef ref = ss.find(1u);
  ASSERT_NE(ref, nullptr);

  for (const DerivKind kind : kAllDerivKinds) {
    const DerivPosition p = deriv_pos_on(kind, 1u, 1.0);
    detail::DerivShock none{};
    const auto base_q =
        detail::deriv_price_shocked_on_ref(ref, p.contract, DerivConfig{}, std::nullopt, none);
    ASSERT_TRUE(base_q.has_value()) << kind_name(kind) << ": " << base_q.error().to_string();

    for (const double dr : {0.0025, -0.0025}) {
      detail::DerivShock shock{};
      shock.rate_shift = dr;
      const auto q =
          detail::deriv_price_shocked_on_ref(ref, p.contract, DerivConfig{}, std::nullopt, shock);
      ASSERT_TRUE(q.has_value()) << kind_name(kind) << ": " << q.error().to_string();
      const double expected = base_q->pv * std::exp(-dr * p.contract.maturity_t);
      EXPECT_NEAR(q->pv, expected, 1.0e-12 * std::abs(expected) + 1.0e-12)
          << kind_name(kind) << " dr=" << dr;
    }
  }
}

// The `dt` axis, where the shocked reprice changes the contract's TENOR -- the
// axis the centre-scheme pin does the most work on and the one round 1 never
// compared. Same O(h^3) convergence claim as the spot/vol sweep, on the axis
// that was untested.
TEST(ScenarioGridDeriv, TaylorAndExactAgreeOnTheTimeAndRateAxesToo) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});
  const std::vector<DerivPosition> derivs = {deriv_pos_on(DerivKind::VarSwap, 1u, 2.0),
                                             deriv_pos_on(DerivKind::VolSwap, 1u, -1.0)};

  // A grid whose only shocks are dt and dr. `spot_pct`/`vol_bump` carry a single
  // zero so the routing predicate is driven purely by the radii.
  const auto gap_at = [&](double scale) {
    ScenarioGridSpec t_spec;
    t_spec.spot_pct = {0.0};
    t_spec.vol_bump = {0.0};
    t_spec.dt = 0.02 * scale;
    t_spec.dr = 0.001 * scale;
    t_spec.taylor_radius_spot = kInf; // never route
    t_spec.taylor_radius_vol = kInf;
    ScenarioGridSpec e_spec = t_spec;
    e_spec.taylor_radius_spot = -1.0; // always route
    e_spec.taylor_radius_vol = -1.0;

    const auto t = scenario_grid({}, derivs, ss, t_spec);
    const auto e = scenario_grid({}, derivs, ss, e_spec);
    EXPECT_TRUE(t.has_value());
    EXPECT_TRUE(e.has_value());
    if (!t.has_value() || !e.has_value()) {
      return std::pair<double, double>{0.0, 0.0};
    }
    EXPECT_EQ(e->deriv_route[0], static_cast<std::uint8_t>(ScenarioRoute::Exact));
    EXPECT_EQ(e->n_deriv_exact_fallback_lanes, 0u) << "every shocked reprice must have solved";
    return std::pair<double, double>{std::abs(t->deriv_pnl[0] - e->deriv_pnl[0]),
                                     std::abs(e->deriv_pnl[0])};
  };

  const auto [gap_h, mag_h] = gap_at(1.0);
  const auto [gap_half, mag_half] = gap_at(0.5);
  (void)mag_half;
  ASSERT_GT(mag_h, 0.0) << "a dt/dr-only grid must still move the book";
  ASSERT_GT(gap_h, 0.0) << "an exactly-zero gap would mean the Exact route never repriced";
  EXPECT_LT(gap_h, 0.02 * mag_h) << "gap=" << gap_h << " magnitude=" << mag_h;
  EXPECT_LT(gap_half, gap_h / 3.0)
      << "the time/rate remainder must shrink super-linearly as the shock halves: " << gap_h
      << " -> " << gap_half;
}

// `time_roll` consuming the whole tenor is a guard with an error message and no
// caller. Reach it, and reach the neighbouring "shock must be finite" refusals,
// so the validation is a tested contract rather than a written one.
TEST(ScenarioGridDeriv, TheShockedRepriceRefusesAnUndefinedShock) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});
  const SurfaceRef ref = ss.find(1u);
  ASSERT_NE(ref, nullptr);
  const DerivPosition p = deriv_pos_on(DerivKind::VarSwap, 1u, 1.0);
  const double nan = std::numeric_limits<double>::quiet_NaN();

  const auto refused = [&](const detail::DerivShock &s) {
    return !detail::deriv_price_shocked_on_ref(ref, p.contract, DerivConfig{}, std::nullopt, s)
                .has_value();
  };

  detail::DerivShock roll_past_expiry{};
  roll_past_expiry.time_roll = p.contract.maturity_t; // exactly consumes the tenor
  EXPECT_TRUE(refused(roll_past_expiry));
  roll_past_expiry.time_roll = p.contract.maturity_t + 1.0;
  EXPECT_TRUE(refused(roll_past_expiry));

  detail::DerivShock bad_spot{};
  bad_spot.spot_rel = -1.0; // the forward would collapse to zero
  EXPECT_TRUE(refused(bad_spot));
  bad_spot.spot_rel = nan;
  EXPECT_TRUE(refused(bad_spot));

  for (double detail::DerivShock::*field :
       {&detail::DerivShock::vol_shift, &detail::DerivShock::skew_shift,
        &detail::DerivShock::convexity_shift, &detail::DerivShock::rate_shift,
        &detail::DerivShock::time_roll}) {
    detail::DerivShock s{};
    s.*field = nan;
    EXPECT_TRUE(refused(s)) << "a non-finite shock component must be refused, not priced";
  }

  // A roll that leaves tenor is fine, and is the positive control for the guard
  // above -- otherwise "refuses everything" would pass every assertion here.
  detail::DerivShock ok_roll{};
  ok_roll.time_roll = p.contract.maturity_t * 0.5;
  EXPECT_FALSE(refused(ok_roll));
}

// The `centre == nullptr` DEFAULT branch: the path a caller who omits the
// argument takes, and the only one that resolves its own centre at the cost of
// an extra strip. Nothing in the tree reached it. It must agree with the
// explicit-centre form to the bit -- they pin off the same centre, one just
// prices it first.
TEST(ScenarioGridDeriv, TheDefaultCentreBranchMatchesAnExplicitlyPassedCentre) {
  const PricedSurface ps = make_essvi(1, 4);
  const SurfaceSet ss = set_of({&ps});
  const SurfaceRef ref = ss.find(1u);
  ASSERT_NE(ref, nullptr);

  DerivGreekBumps bumps{};
  bumps.smile_greeks = true;

  for (const DerivKind kind : kAllDerivKinds) {
    const DerivPosition p = deriv_pos_on(kind, 1u, 1.0);
    const auto g = detail::deriv_greeks_on_ref(ref, p.contract, DerivConfig{}, bumps, std::nullopt);
    ASSERT_TRUE(g.has_value()) << kind_name(kind) << ": " << g.error().to_string();

    detail::DerivShock shock{};
    shock.spot_rel = 0.02;
    shock.vol_shift = 0.01;
    shock.time_roll = 0.01;
    shock.rate_shift = 0.0005;

    const auto with_centre = detail::deriv_price_shocked_on_ref(ref, p.contract, DerivConfig{},
                                                                std::nullopt, shock, &g->quote);
    const auto defaulted =
        detail::deriv_price_shocked_on_ref(ref, p.contract, DerivConfig{}, std::nullopt, shock);
    ASSERT_TRUE(with_centre.has_value()) << kind_name(kind);
    ASSERT_TRUE(defaulted.has_value()) << kind_name(kind);
    EXPECT_TRUE(bits_equal(with_centre->pv, defaulted->pv))
        << kind_name(kind) << ": passing the centre must be an optimisation, not a semantic "
        << "choice -- " << with_centre->pv << " vs " << defaulted->pv;
  }
}
