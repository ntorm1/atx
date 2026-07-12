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
      const bool exact =
          std::abs(spec.spot_pct[i]) > 0.05 || std::abs(spec.vol_bump[j]) > 0.03;
      const std::size_t c = i * r->n_vol + j;
      EXPECT_EQ(r->route[c], static_cast<std::uint8_t>(exact ? ScenarioRoute::Exact
                                                             : ScenarioRoute::Taylor))
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

// ── C3.2-5. Taylor and Exact agree INSIDE the declared radii (§9.3 gate). ─────
TEST(ScenarioGrid, TaylorExactAgreeInsideRadius) {
  const PricedSurface base = make_essvi(1, 5);
  const SurfaceSet bset = set_of({&base});
  // Axes AT/inside the default radii — every cell would route Taylor by default.
  const std::vector<double> spot = {-kDefaultTaylorRadiusSpot, -0.01, 0.0, 0.01,
                                    kDefaultTaylorRadiusSpot};
  const std::vector<double> vol = {-kDefaultTaylorRadiusVol, -0.01, 0.0, 0.01,
                                   kDefaultTaylorRadiusVol};
  double worst = 0.0;
  for (double K : {90.0, 95.0, 100.0, 105.0, 110.0}) {
    for (double T : {0.15, 0.25, 0.35}) {
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
  // The MEASURED (req 4) $0.005 band is a PER-AXIS bound (pure-spot / pure-vol sweeps).
  // With OR routing, the worst Taylor cell inside the radii is the DOUBLE CORNER
  // (|spot|=rad AND |vol|=rad simultaneously), whose combined higher-order + vanna
  // cross-term residual reaches ~$0.0091 — above the per-axis band. That measured
  // corner value is the declared §9.3 agreement tolerance, pinned here.
  EXPECT_LE(worst, 1.0e-2);
}

// ── C3.2-6. Second-order convergence: residual scales ~1/8 as h -> h/2. ──────
TEST(ScenarioGrid, SecondOrderConvergence) {
  // Higher-accuracy AL preset so the exact reprice residual is not solver-noise
  // limited at the smaller step.
  const AlOpts acc{12, 48, 12, 1.0e-12};
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
    const double expected = scenario_taylor_leg(scaled_greeks(s, book.front()), dS, 0.0, 0.0, -0.05);
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
  EXPECT_EQ(kDefaultTaylorRadiusSpot, 0.03); // MEASURED (req 4) — see task-c3.2-report.md
  EXPECT_EQ(kDefaultTaylorRadiusVol, 0.03);  // MEASURED (req 4)
  ScenarioGridSpec s;
  EXPECT_EQ(s.taylor_radius_spot, kDefaultTaylorRadiusSpot);
  EXPECT_EQ(s.taylor_radius_vol, kDefaultTaylorRadiusVol);
}

// ── C3.2-10. Measure the Taylor-valid radius (informational; answers req 4). ──
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
