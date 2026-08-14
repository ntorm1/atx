// PreparedPortfolio suite — proves the substrate is a stable, aligned, grouped
// PERMUTATION of a Portfolio's unique contracts, and that wiring it into
// PortfolioPricer::price() leaves the frame BIT-FOR-BIT identical.
//
// Coverage:
//   * original_contract_index() is a bijection onto [0, n_unique) and applying it
//     to the permuted SoA recovers Portfolio::contracts() exactly;
//   * groups() partitions [0, n_unique) (no gap/overlap), every group is
//     homogeneous in (uid, side), members contiguous;
//   * equal-T runs within a (uid, side) group are contiguous (ladder-ready);
//   * stability: distinct-T entries of one (uid, side) come out ascending-T; equal
//     (uid, side, T) entries (distinct strikes) keep first-appearance order;
//   * the aligned columns are actually 64-byte aligned;
//   * the grouped price() equals an INDEPENDENT per-contract oracle bit-for-bit on
//     a 64-uid / multi-expiry / mixed-side book, and its whole-frame fingerprint
//     matches a golden captured from the pre-change price() (the pin).

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/backtest/portfolio_pricer.hpp"
#include "backtest/prepared_portfolio.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/fitting/vol_surface.hpp"
#include "support/isa_golden_tol.hpp"

using namespace atx::vol;

namespace {

constexpr double kS = 100.0;
constexpr double kR = 0.043;
constexpr std::int64_t kNow = 1700000000000000000LL;

[[nodiscard]] std::uint64_t bits(double x) noexcept {
  std::uint64_t b = 0;
  std::memcpy(&b, &x, sizeof b);
  return b;
}
[[nodiscard]] bool bits_equal(double a, double b) noexcept { return bits(a) == bits(b); }

[[nodiscard]] PricingContext make_pricing(std::uint32_t uid) {
  PricingContext pc;
  pc.S = kS;
  pc.r = kR;
  pc.now_ts_ns = kNow;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return pc;
}

[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.12 * static_cast<double>(i);
    EssviParams e{};
    e.theta = 0.04 + 0.006 * static_cast<double>(i);
    e.phi = 1.4 - 0.04 * static_cast<double>(i);
    e.rho = -0.35 + 0.015 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = kS;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, kS, 0.0, 0.02, 200, 5});
  }
  return PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid)).value();
}

[[nodiscard]] PricedSurface make_convex(std::uint32_t uid, int n, int nodes) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.12 * static_cast<double>(i);
    const double df = std::exp(-kR * T);
    const double sigma = 0.18 + 0.01 * static_cast<double>(i);
    ConvexSliceFit fit;
    fit.T = T;
    fit.F = kS;
    fit.df = df;
    fit.rmse_price = 0.3;
    fit.n_obs = static_cast<std::size_t>(nodes);
    fit.n_active = 4;
    fit.u.resize(static_cast<std::size_t>(nodes));
    fit.C.resize(static_cast<std::size_t>(nodes));
    for (int j = 0; j < nodes; ++j) {
      const double K = kS * (0.7 + 0.6 * static_cast<double>(j) / static_cast<double>(nodes - 1));
      fit.u[static_cast<std::size_t>(j)] = K;
      fit.C[static_cast<std::size_t>(j)] = black76_price(kS, K, T, sigma, df, Side::Call);
    }
    cs.push(std::make_unique<ConvexDenseCurve>(std::move(fit)));
    ctx.push_back(SliceContext{T, kS, 0.0, 0.02, static_cast<std::size_t>(nodes), 3});
  }
  return PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid)).value();
}

// A structurally-rich small book: 3 uids, both sides, several expiries, a dup, a
// scrambled-strike run (to exercise the tie-break), a missing uid, a degenerate.
[[nodiscard]] std::vector<Position> mixed_book() {
  std::vector<Position> book;
  std::uint64_t id = 0;
  // uid 2, put, one expiry, strikes deliberately OUT OF ORDER (tie-break test):
  for (double K : {104.0, 96.0, 100.0, 92.0}) {
    book.push_back({id++, {2, K, 0.30, Side::Put}, -3.0, 100.0});
  }
  // uid 1, both sides, two expiries in DESCENDING input order (ascending-T test):
  for (double T : {0.29, 0.17}) {
    for (double K : {98.0, 105.0}) {
      book.push_back({id++, {1, K, T, Side::Call}, +4.0, 100.0});
      book.push_back({id++, {1, K, T, Side::Put}, -2.0, 100.0});
    }
  }
  // uid 3, calls, one expiry.
  for (double K : {100.0, 110.0}) {
    book.push_back({id++, {3, K, 0.18, Side::Call}, +1.0, 100.0});
  }
  // A duplicate of an existing contract (same bits) with a different qty.
  book.push_back({id++, {1, 98.0, 0.17, Side::Call}, +9.0, 100.0});
  // A missing-uid position and a degenerate contract.
  book.push_back({id++, {999, 100.0, 0.10, Side::Call}, +1.0, 100.0});
  book.push_back({id++, {1, -5.0, 0.0, Side::Put}, +1.0, 100.0}); // K<=0 && T<=0
  return book;
}

}  // namespace

// ── Substrate structure ──────────────────────────────────────────────────────

TEST(PreparedPortfolio, ReversePermutationIsBijectionRecoveringContracts) {
  auto pf = Portfolio::create(mixed_book());
  ASSERT_TRUE(pf.has_value());
  auto pp = PreparedPortfolio::create(*pf, PriceOptions{});
  ASSERT_TRUE(pp.has_value());

  const std::size_t n = pp->n_unique();
  EXPECT_EQ(n, pf->n_contracts());

  const auto oci = pp->original_contract_index();
  ASSERT_EQ(oci.size(), n);
  std::set<std::uint32_t> seen;
  for (std::size_t p = 0; p < n; ++p) {
    EXPECT_LT(oci[p], n);           // maps into range
    EXPECT_TRUE(seen.insert(oci[p]).second) << "duplicate original index at " << p;
  }
  EXPECT_EQ(seen.size(), n);        // onto: a true permutation

  // Applying the reverse permutation to the permuted SoA recovers contracts().
  const auto contracts = pf->contracts();
  for (std::size_t p = 0; p < n; ++p) {
    const OptionContract& c = contracts[oci[p]];
    EXPECT_TRUE(bits_equal(pp->k()[p], c.K)) << p;
    EXPECT_TRUE(bits_equal(pp->t()[p], c.T)) << p;
    EXPECT_EQ(pp->uid()[p], c.uid) << p;
    EXPECT_EQ(pp->side()[p], c.side) << p;
  }
}

TEST(PreparedPortfolio, TenorRefreshRejectsEqualityMergeWithoutMutation) {
  const std::vector<Position> positions{
      {1u, {1u, 90.0, 0.10, Side::Call}, +1.0, 100.0},
      {2u, {1u, 100.0, 0.20, Side::Call}, +1.0, 100.0},
      {3u, {1u, 110.0, 0.30, Side::Call}, +1.0, 100.0},
  };
  auto pf = Portfolio::create(positions);
  ASSERT_TRUE(pf.has_value()) << pf.error().to_string();
  auto prepared = PreparedPortfolio::create(*pf, PriceOptions{});
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();
  const std::vector<double> original_t(prepared->t().begin(), prepared->t().end());
  std::vector<std::uint64_t> original_tile_t;
  original_tile_t.reserve(prepared->price_tiles().size());
  for (const PreparedPriceTile& tile : prepared->price_tiles()) {
    original_tile_t.push_back(tile.t_bits);
  }

  const std::array<double, 3> merged_t{0.10, 0.20, 0.20};
  ASSERT_TRUE(pf->retime(merged_t).has_value());
  EXPECT_FALSE(prepared->try_refresh_tenors(*pf));

  ASSERT_EQ(prepared->t().size(), original_t.size());
  for (std::size_t i = 0; i < original_t.size(); ++i) {
    EXPECT_TRUE(bits_equal(prepared->t()[i], original_t[i])) << i;
  }
  ASSERT_EQ(prepared->price_tiles().size(), original_tile_t.size());
  for (std::size_t i = 0; i < original_tile_t.size(); ++i) {
    EXPECT_EQ(prepared->price_tiles()[i].t_bits, original_tile_t[i]) << i;
  }
}

TEST(PreparedPortfolio, GroupsPartitionAndAreHomogeneous) {
  auto pf = Portfolio::create(mixed_book());
  ASSERT_TRUE(pf.has_value());
  auto pp = PreparedPortfolio::create(*pf, PriceOptions{});
  ASSERT_TRUE(pp.has_value());
  const std::size_t n = pp->n_unique();

  const auto groups = pp->groups();
  ASSERT_FALSE(groups.empty());
  // Tile [0, n): first begins at 0, each begins where the last ended, last ends n.
  std::uint32_t cursor = 0;
  for (const ContractGroup& g : groups) {
    EXPECT_EQ(g.begin, cursor) << "gap/overlap in partition";
    EXPECT_LT(g.begin, g.end) << "empty group";
    // Homogeneous in (uid, side); members contiguous.
    for (std::uint32_t p = g.begin; p < g.end; ++p) {
      EXPECT_EQ(pp->uid()[p], g.uid) << p;
      EXPECT_EQ(pp->side()[p], g.side) << p;
    }
    cursor = g.end;
  }
  EXPECT_EQ(cursor, n);
}

TEST(PreparedPortfolio, PriceTilesAreDeterministicRawExpiryHomogeneousPartition) {
  std::vector<Position> book;
  std::uint64_t id = 0;
  const double T0 = 0.25;
  const double T1 = std::nextafter(T0, 1.0);
  const auto add_run = [&](std::uint32_t uid, Side side, double T, int n, double k0) {
    for (int i = 0; i < n; ++i) {
      book.push_back({id++, {uid, k0 + 0.125 * static_cast<double>(i), T, side}, 1.0, 100.0});
    }
  };
  add_run(1, Side::Call, T0, 70, 70.0);
  add_run(1, Side::Call, T1, 5, 90.0);
  add_run(1, Side::Put, T0, 66, 80.0);
  add_run(2, Side::Call, T0, 3, 95.0);

  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  auto a = PreparedPortfolio::create(
      *pf, PriceOptions{.n_threads = 8, .resolved_price_isa = simd::SimdIsa::ForceAvx2});
  auto b = PreparedPortfolio::create(
      *pf, PriceOptions{.n_threads = 1, .prices_only = true,
                        .resolved_price_isa = simd::SimdIsa::ForceScalar});
  ASSERT_TRUE(a.has_value() && b.has_value());
  const auto at = a->price_tiles();
  const auto bt = b->price_tiles();
  ASSERT_EQ(at.size(), 6u);
  ASSERT_EQ(bt.size(), at.size());

  std::uint32_t cursor = 0;
  for (std::size_t i = 0; i < at.size(); ++i) {
    const PreparedPriceTile& tile = at[i];
    EXPECT_EQ(tile.begin, cursor);
    EXPECT_LT(tile.begin, tile.end);
    EXPECT_LE(tile.end - tile.begin, kPreparedPriceTileLanes);
    for (std::uint32_t p = tile.begin; p < tile.end; ++p) {
      EXPECT_EQ(a->uid()[p], tile.uid) << p;
      EXPECT_EQ(a->side()[p], tile.side) << p;
      EXPECT_EQ(bits(a->t()[p]), tile.t_bits) << p;
    }
    if (i + 1 < at.size() && at[i + 1].uid == tile.uid &&
        at[i + 1].side == tile.side && at[i + 1].t_bits == tile.t_bits) {
      EXPECT_EQ(tile.end - tile.begin, kPreparedPriceTileLanes);
    }
    EXPECT_EQ(bt[i].uid, tile.uid);
    EXPECT_EQ(bt[i].side, tile.side);
    EXPECT_EQ(bt[i].t_bits, tile.t_bits);
    EXPECT_EQ(bt[i].begin, tile.begin);
    EXPECT_EQ(bt[i].end, tile.end);
    cursor = tile.end;
  }
  EXPECT_EQ(cursor, a->n_unique());
}

TEST(PreparedPortfolio, GreekTilesAreBookDeterminedGroupHomogeneousPartition) {
  std::vector<Position> book;
  std::uint64_t id = 0;
  const double T0 = 0.25;
  const double T1 = std::nextafter(T0, 1.0);
  const auto add_run = [&](std::uint32_t uid, Side side, double T, int n, double k0) {
    for (int i = 0; i < n; ++i) {
      book.push_back({id++, {uid, k0 + 0.125 * static_cast<double>(i), T, side}, 1.0, 100.0});
    }
  };
  add_run(1, Side::Call, T0, 70, 70.0);
  add_run(1, Side::Call, T1, 5, 90.0);
  add_run(1, Side::Put, T0, 66, 80.0);
  add_run(2, Side::Call, T0, 3, 95.0);

  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  auto a =
      PreparedPortfolio::create(*pf, PriceOptions{.n_threads = 8,
                                                  .analytic_greeks = true,
                                                  .resolved_price_isa = simd::SimdIsa::ForceAvx2});
  auto b = PreparedPortfolio::create(
      *pf, PriceOptions{.n_threads = 1,
                        .adjoint_greeks = true,
                        .prices_only = true,
                        .skew_adjusted_delta = true,
                        .resolved_price_isa = simd::SimdIsa::ForceScalar,
                        .query_execution = QueryExecution::ColdReference});
  ASSERT_TRUE(a.has_value() && b.has_value());
  const auto at = a->greek_tiles();
  const auto bt = b->greek_tiles();
  ASSERT_EQ(at.size(), 5u);
  ASSERT_EQ(bt.size(), at.size());

  bool saw_mixed_expiry_tile = false;
  std::uint32_t cursor = 0;
  for (std::size_t i = 0; i < at.size(); ++i) {
    const PreparedGreekTile &tile = at[i];
    EXPECT_EQ(tile.begin, cursor);
    EXPECT_LT(tile.begin, tile.end);
    EXPECT_LE(tile.end - tile.begin, kPreparedGreekTileLanes);

    const std::uint64_t first_t_bits = bits(a->t()[tile.begin]);
    for (std::uint32_t p = tile.begin; p < tile.end; ++p) {
      EXPECT_EQ(a->uid()[p], tile.uid) << p;
      EXPECT_EQ(a->side()[p], tile.side) << p;
      saw_mixed_expiry_tile = saw_mixed_expiry_tile || bits(a->t()[p]) != first_t_bits;
    }
    if (i + 1 < at.size() && at[i + 1].uid == tile.uid && at[i + 1].side == tile.side) {
      EXPECT_EQ(tile.end - tile.begin, kPreparedGreekTileLanes);
    }

    EXPECT_EQ(bt[i].uid, tile.uid);
    EXPECT_EQ(bt[i].side, tile.side);
    EXPECT_EQ(bt[i].begin, tile.begin);
    EXPECT_EQ(bt[i].end, tile.end);
    cursor = tile.end;
  }
  EXPECT_EQ(cursor, a->n_unique());
  EXPECT_TRUE(saw_mixed_expiry_tile)
      << "Greek tiles must not subdivide a (uid, side) group at raw-T boundaries";
}

TEST(PreparedPortfolio, EqualExpiryRunsWithinGroupAreContiguousAndAscending) {
  auto pf = Portfolio::create(mixed_book());
  ASSERT_TRUE(pf.has_value());
  auto pp = PreparedPortfolio::create(*pf, PriceOptions{});
  ASSERT_TRUE(pp.has_value());

  for (const ContractGroup& g : pp->groups()) {
    // Within a group, T is non-decreasing, so equal-bit-T entries are contiguous
    // (a bracket-reuse ladder) and never interleave with a different T.
    double prev = pp->t()[g.begin];
    for (std::uint32_t p = g.begin + 1; p < g.end; ++p) {
      const double cur = pp->t()[p];
      EXPECT_LE(prev, cur) << "T not ascending within group at slot " << p;
      prev = cur;
    }
  }
}

TEST(PreparedPortfolio, StableAscendingTAndFirstAppearanceTieBreak) {
  auto pf = Portfolio::create(mixed_book());
  ASSERT_TRUE(pf.has_value());
  auto pp = PreparedPortfolio::create(*pf, PriceOptions{});
  ASSERT_TRUE(pp.has_value());
  const auto contracts = pf->contracts();
  const auto oci = pp->original_contract_index();

  // The uid-2 put run: four strikes at one T, given out of order. After sort they
  // must be a single contiguous run in FIRST-APPEARANCE (ascending original index)
  // order — the tie-break, since (uid, side, T) are all equal.
  for (const ContractGroup& g : pp->groups()) {
    if (g.uid == 2 && g.side == Side::Put) {
      std::uint32_t prev_orig = 0;
      bool first = true;
      for (std::uint32_t p = g.begin; p < g.end; ++p) {
        // All share T == 0.30.
        EXPECT_TRUE(bits_equal(pp->t()[p], 0.30));
        if (!first) {
          EXPECT_LT(prev_orig, oci[p]) << "tie not broken by ascending original index";
        }
        prev_orig = oci[p];
        first = false;
      }
    }
    // The uid-1 group's two expiries were input descending (0.29 then 0.17); the
    // permuted order must be ascending T (0.17 before 0.29).
    if (g.uid == 1 && g.side == Side::Call) {
      double prev = -1.0;
      for (std::uint32_t p = g.begin; p < g.end; ++p) {
        EXPECT_LE(prev, pp->t()[p]);
        prev = pp->t()[p];
      }
      // The scrambled group must be non-trivial (contains both expiries).
      EXPECT_GE(g.end - g.begin, 3u);
    }
  }
  (void)contracts;
}

TEST(PreparedPortfolio, AlignedColumnsAre64ByteAligned) {
  auto pf = Portfolio::create(mixed_book());
  ASSERT_TRUE(pf.has_value());
  auto pp = PreparedPortfolio::create(*pf, PriceOptions{});
  ASSERT_TRUE(pp.has_value());
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(pp->k().data()) % 64u, 0u);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(pp->t().data()) % 64u, 0u);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(pp->uid().data()) % 64u, 0u);
}

// ── The pin: grouped price() == independent oracle, bit-for-bit ───────────────

namespace {

// Build a 64-uid, mixed-kind, 6-expiry, mixed-side book (matches the bench shape)
// plus a dup, a missing uid, and a degenerate — the representative frame.
struct Rig {
  std::vector<PricedSurface> surfs;
  std::vector<Position> book;
};

[[nodiscard]] const PricedSurface* find_surf(const std::vector<PricedSurface>& surfs,
                                             std::uint32_t uid) {
  for (const PricedSurface& s : surfs) {
    if (s.uid() == uid) {
      return &s;
    }
  }
  return nullptr;
}

// FNV-1a over the full frame (every numeric column's raw bits + status + totals) —
// a compact fingerprint of the whole PriceFrame.
[[nodiscard]] std::uint64_t fingerprint(const PriceFrame& f) noexcept {
  std::uint64_t h = 1469598103934665603ULL;
  auto mix = [&h](std::uint64_t v) {
    for (int b = 0; b < 8; ++b) {
      h ^= (v >> (8 * b)) & 0xFFu;
      h *= 1099511628211ULL;
    }
  };
  auto mixd = [&](double d) { mix(bits(d)); };
  for (std::size_t i = 0; i < f.size(); ++i) {
    mix(f.id[i]);
    mix(f.uid[i]);
    mixd(f.pv[i]);
    mixd(f.price[i]);
    mixd(f.iv[i]);
    mixd(f.delta[i]);
    mixd(f.gamma[i]);
    mixd(f.vega[i]);
    mixd(f.theta[i]);
    mixd(f.rho[i]);
    mixd(f.vanna[i]);
    mixd(f.volga[i]);
    mixd(f.charm[i]);
    mix(static_cast<std::uint64_t>(f.status[i]));
  }
  mixd(f.total.pv);
  mixd(f.total.delta);
  mixd(f.total.gamma);
  mixd(f.total.vega);
  mixd(f.total.theta);
  mixd(f.total.rho);
  mixd(f.total.vanna);
  mixd(f.total.volga);
  mixd(f.total.charm);
  mix(f.total.n_ok);
  return h;
}

}  // namespace

TEST(PreparedPortfolio, GroupedPriceEqualsIndependentOracleAndPinnedFingerprint) {
  constexpr int kUnderlyings = 64;
  constexpr int kSlices = 6;
  Rig rig;
  rig.surfs.reserve(kUnderlyings);
  for (int u = 1; u <= kUnderlyings; ++u) {
    rig.surfs.push_back((u & 1) ? make_convex(static_cast<std::uint32_t>(u), kSlices, 40)
                                : make_essvi(static_cast<std::uint32_t>(u), kSlices));
  }
  std::vector<const PricedSurface*> ptrs;
  for (const PricedSurface& s : rig.surfs) {
    ptrs.push_back(&s);
  }
  auto surfaces = SurfaceSet::create(ptrs);
  ASSERT_TRUE(surfaces.has_value());

  std::uint64_t id = 0;
  for (int u = 1; u <= kUnderlyings; ++u) {
    for (int i = 0; i < kSlices; ++i) {
      const double T = 0.05 + 0.12 * static_cast<double>(i);
      for (double K : {85.0, 92.0, 98.0, 100.0, 102.0, 108.0, 115.0}) {
        const Side side = (K <= 100.0) ? Side::Put : Side::Call;
        rig.book.push_back({id++, {static_cast<std::uint32_t>(u), K, T, side}, 5.0, 100.0});
      }
    }
  }
  // A dup (same contract as the very first position), a missing uid, a degenerate.
  rig.book.push_back({id++, {1, 85.0, 0.05, Side::Put}, -2.0, 100.0});
  rig.book.push_back({id++, {999, 100.0, 0.2, Side::Call}, 1.0, 100.0});
  rig.book.push_back({id++, {2, 0.0, 0.0, Side::Call}, 1.0, 100.0});

  auto pf = Portfolio::create(rig.book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));

  auto fr = pricer.price(*surfaces, PriceOptions{.n_threads = 4});
  ASSERT_TRUE(fr.has_value());
  const PriceFrame& f = *fr;
  ASSERT_EQ(f.size(), rig.book.size());

  // Independent oracle: price each position from direct PricedSurface queries
  // (the SAME reference the MultiKind guard uses), replicating the pricer's exact
  // status + iv policy.
  auto degenerate = [](const OptionContract& c) {
    return !(std::isfinite(c.K) && c.K > 0.0 && std::isfinite(c.T) && c.T > 0.0);
  };
  for (std::size_t i = 0; i < rig.book.size(); ++i) {
    const Position& p = rig.book[i];
    const OptionContract& c = p.contract;
    EXPECT_EQ(f.id[i], p.id);
    EXPECT_EQ(f.uid[i], c.uid);
    if (degenerate(c)) {
      EXPECT_EQ(f.status[i], PriceStatus::InvalidContract) << i;
      EXPECT_TRUE(bits_equal(f.iv[i], 0.0)) << i;
      continue;
    }
    const PricedSurface* s = find_surf(rig.surfs, c.uid);
    if (s == nullptr) {
      EXPECT_EQ(f.status[i], PriceStatus::ModelUnavailable) << i;
      EXPECT_TRUE(bits_equal(f.iv[i], 0.0)) << i;
      continue;
    }
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    const auto g = s->greeks(c.K, c.T, c.side);
    const auto fv = s->fair_value(c.K, c.T, c.side);
    ASSERT_TRUE(g.has_value() && fv.has_value());
    const double w = p.qty * p.multiplier;
    EXPECT_TRUE(bits_equal(f.price[i], *fv)) << i;
    EXPECT_TRUE(bits_equal(f.iv[i], s->iv(c.K, c.T))) << i;
    EXPECT_TRUE(bits_equal(f.pv[i], w * *fv)) << i;
    EXPECT_TRUE(bits_equal(f.delta[i], w * g->delta)) << i;
    EXPECT_TRUE(bits_equal(f.gamma[i], w * g->gamma)) << i;
    EXPECT_TRUE(bits_equal(f.vega[i], w * g->vega)) << i;
    EXPECT_TRUE(bits_equal(f.theta[i], w * g->theta)) << i;
    EXPECT_TRUE(bits_equal(f.rho[i], w * g->rho)) << i;
    EXPECT_TRUE(bits_equal(f.vanna[i], w * g->vanna)) << i;
    EXPECT_TRUE(bits_equal(f.volga[i], w * g->volga)) << i;
    EXPECT_TRUE(bits_equal(f.charm[i], w * g->charm)) << i;
  }

  // Thread-count invariance of the grouped solve (belt-and-suspenders alongside
  // the existing Price_ThreadCounts guard).
  auto fr1 = pricer.price(*surfaces, PriceOptions{.n_threads = 1});
  auto fr8 = pricer.price(*surfaces, PriceOptions{.n_threads = 8});
  ASSERT_TRUE(fr1.has_value() && fr8.has_value());
  const std::uint64_t h4 = fingerprint(f);
  EXPECT_EQ(h4, fingerprint(*fr1));
  EXPECT_EQ(h4, fingerprint(*fr8));

  // The pin: a whole-frame bit fingerprint. It guards the grouped substrate against
  // the per-contract evaluate() path (thread-invariance above is the live gate).
  // A1 REPIN (core-review finding 1): the American book reprices through the cold
  // andersen_lake path whose BAW seed sign was fixed, shifting the marks ~1e-6 and
  // thus every hashed bit — the FNV fingerprint moves wholesale (a hash has no
  // "small" delta). Grouped==oracle economic equality above is unchanged.
  //
  // ISA-KEYED PIN (pg-sota sprint, 2026-07): the SSE2 value below was captured on
  // the dev preset (source-of-truth ISA). Under FMA contraction (the rel-avx2
  // acceptance preset, /arch:AVX2 → -mfma) the American book reprices ~1 ULP through
  // the fused `a*b+c` in andersen_lake, and because a whole-frame FNV hash admits NO
  // tolerance band — a single last-place bit rehashes the entire frame — the pin
  // moves wholesale to a distinct-but-correct value. A per-ISA `golden_close` band
  // (as BoundaryHoist uses for its scalar pins) is therefore meaningless on a hash.
  // So we pin BOTH goldens and select on the active ISA (kFmaContraction, from
  // support/isa_golden_tol.hpp): the SSE2 gate keeps its exact byte-identity
  // guarantee untouched, and rel-avx2 stays a green acceptance gate. Neither pin is
  // loosened; both are exact equalities on their own ISA. See the FMA-divergence
  // rationale in support/isa_golden_tol.hpp.
  // MERGE RE-PIN (feat/pg-sota -> main, 2026-07-20): the merged tree combines
  // main's fixture/pricing state (incl. the S4 migration + parallel opra/greeks
  // work) with the sprint's A1 BAW-seed sign fix. The priced marks therefore differ
  // from EITHER branch alone, so the whole-frame FNV hash moves wholesale to a new
  // correct value on each ISA. The grouped==independent-oracle economic-parity and
  // thread-invariance gates above stayed green through the re-pin — only the hash of
  // the (legitimately shifted) marks moved. Values measured on the merged tree:
  // SSE2 on `rel` preset, FMA on `rel-avx2`.
  // PIPELINE-M RE-PIN (feat/disp-hotpath -> feat/pipeline-m, 2026-07-21): the M1
  // keystone merge folded disp-hotpath's auto-merged pricing changes onto main's A9
  // greeks kernel, moving the whole-frame FNV hash on the SSE2/dev route wholesale
  // 17305682487856730537 -> 718570745730299145. The grouped==independent-oracle
  // economic-parity and thread-invariance gates above stayed green (differences are
  // inside the documented tolerance), so only the hash of the legitimately-shifted
  // marks moved. The FMA/rel-avx2 pin was RE-VERIFIED unchanged on the merged tree.
  // CONFIG-KEYED PIN (merge of main, 2026-08-02; controller ruling, closeout Task 0).
  // The two SSE2 values above were never rival captures of different trees — they are
  // this ONE tree's Release and Debug values, and the file has been oscillating between
  // them. Read the two paragraphs above literally: the 2026-07-20 re-pin says its SSE2
  // number was "measured on the `rel` preset", the 2026-07-21 re-pin says its number
  // moved "on the SSE2/dev route". Each re-pin therefore fixed one preset and silently
  // broke the other, because `kFmaContraction` keys on ISA alone and this fixture also
  // diverges Debug-vs-Release (Release reassociates and contracts the andersen_lake
  // arithmetic; a whole-frame FNV hash has no tolerance band, so one last-place bit
  // rehashes everything). The axis is now explicit.
  //
  // Precedent for keying on config: the NAV determinism anchors are already per-preset,
  // `american_test.cpp:2686` splits BoundaryHoist's ATM put on NDEBUG, and
  // `multiname_pipeline_test.cpp:923` splits its E1 baselines the same way.
  //
  // PROVENANCE — measured on THIS tree (branch feat/vol-v1-release, 56df9cc, 2026-08-02),
  // twice per preset, with the grouped==independent-oracle parity and worker-count
  // invariance gates above green in every case:
  //     dev      (Debug,   SSE2) -> 718570745730299145
  //     rel      (Release, SSE2) -> 17305682487856730537
  //     rel-avx2 (Release, FMA)  -> 8754310291975640041
  //
  // LTO NOTE: this provenance predates the LTO flip. `rel`/`rel-avx2` gained
  // CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON at `63f6f29` (v1 closeout sprint Task 2),
  // which verified the SSE2 pin above (`17305682487856730537`) bit-identical
  // LTO-on vs LTO-off on `rel`, so it did not need re-measuring.
  //
  // The FMA branch carries ONE value deliberately: `rel-avx2` is the only preset that
  // injects /arch:AVX2 (CMakePresets.json:84-85) and it inherits `rel`, so __FMA__
  // implies NDEBUG in the shipped preset set and a Debug+FMA cell is unreachable. If a
  // Debug+AVX2 preset is ever added, SPLIT this branch and capture it — do not let it
  // fall through to a Release-captured number.
#if defined(NDEBUG)
  constexpr std::uint64_t kGoldenFingerprintSse2 = 17305682487856730537ULL;
#else
  constexpr std::uint64_t kGoldenFingerprintSse2 = 718570745730299145ULL;
#endif
  constexpr std::uint64_t kGoldenFingerprintFma = 8754310291975640041ULL;
  constexpr std::uint64_t kGoldenFingerprint =
      atx::vol::test::kFmaContraction ? kGoldenFingerprintFma : kGoldenFingerprintSse2;
  EXPECT_EQ(h4, kGoldenFingerprint);
}
