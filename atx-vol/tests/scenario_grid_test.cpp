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

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/scenario_grid.hpp"
#include "atx/vol/vol_curve.hpp"

using namespace atx::vol;

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
                                          std::int64_t now = kNow) {
  PricingContext pc;
  pc.S = S;
  pc.r = r;
  pc.now_ts_ns = now;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return pc;
}

// eSSVI priced surface (positive carry => genuine American premium on both sides).
[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n, double S = kS, double r = kR,
                                       double q_eff = 0.02) {
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
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid, S, r));
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
