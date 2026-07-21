// spy_dispersion_pnl gate tests (WS-D D4).
//
// The vega-flat dispersion PnL-track strategy: long top-N 40Δ strangles vs a
// short index 40Δ strangle sized net-vega-zero at entry, a NEW clip every day,
// DAILY delta-hedged, HELD TO EXPIRY. Strikes/expiries are resolved off the
// serialized surface (projection path). These gates drive the SAME library
// pipeline examples/spy_dispersion_pnl.cpp composes (SurfaceDb ->
// Clock::from_surface_db -> make_dispersion_strangle_spec{hold_to_expiry,hedge}
// -> DeclarativeStrategy -> run_backtest), NOT the example binary. Fixture: a
// synthetic SurfaceDb (7 fake names + "SPY"), daily partitions, distinct
// per-symbol vol bumps/spots. Integer tenor_days over a daily clock make each
// cohort's expiry land EXACTLY on a later clock date, so held-to-expiry
// settlement is observed exactly (no NotFound).
//
//   1. SpecShape_HoldToExpiry_Hedged  — hold_to_expiry -> Holding::HoldToExpiry;
//      hedge -> DeltaToZero daily; FlatVega constraint still wired.
//   2. FortyDeltaReprice              — every leg resolved off the FIRST db
//      snapshot reprices to |delta| ~ 0.40 (projection path, call K>F, put K<F).
//   3. VegaFlatAtEntry                — net entry-cohort vega ~ 0 on every date
//      (FlatVega scale is exact in fp).
//   4. HeldToExpirySettles           — cohorts held to expiry settle at intrinsic
//      on their aligned expiry date (pnl_settlement fires); the CloseAtHorizon
//      control never settles (pnl_settlement == 0 every row).
//   5. DailyDeltaHedgeBandsNetDelta   — DeltaToZero band 0 -> post-hedge net book
//      delta (gross_delta) ~ 0 every row; the unhedged control is materially
//      exposed.
//   6. AttributionClosureIdentity     — economic sanity: total_return == Σ of the
//      attribution axes incl. settlement/shares/financing/cost (tearsheet gate).
//   7. Determinism_TwoRunAndThreads   — 2-run bit-identity AND 1-vs-4-thread
//      bit-identity over the full hedged, held-to-expiry run.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/american.hpp"            // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"            // Clock, MarketSnapshot, run_backtest, RunConfig
#include "atx/vol/dispersion.hpp"          // MissingNamePolicy, MissingNameSpec
#include "atx/vol/dispersion_strangle.hpp" // DispersionStrangleConfig, make_dispersion_strangle_spec
#include "atx/vol/priced_surface.hpp"      // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"            // DeclarativeStrategy, resolve_spec_with_policy
#include "atx/vol/surface_archive.hpp"     // SurfaceArchiveItem
#include "atx/vol/surface_db.hpp"          // SurfaceDb
#include "atx/vol/surface_parity.hpp"      // SliceContext
#include "atx/vol/tearsheet.hpp"           // TearSheet, tearsheet
#include "atx/vol/types.hpp"               // Result, ErrorCode
#include "atx/vol/vol_curve.hpp"           // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"         // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kDayNs = 86'400'000'000'000LL; // one calendar day; matches kNsPerYear/365.25

// A synthetic eSSVI PricedSurface (flat forward == spot, genuine American
// premium via q_eff=0.02), 7 slices T in [0.05, 1.0]. Copied from
// mag7_dispersion_backtest_test.cpp's make_surface.
[[nodiscard]] PricedSurface make_surface(double S, std::int64_t now_ts, double vol_bump,
                                         std::uint32_t uid) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump;
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = S;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, S, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kR;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value()) << (ps.has_value() ? std::string{} : ps.error().to_string());
  return std::move(*ps);
}

[[nodiscard]] fs::path test_root(std::string_view name) {
  auto p = fs::temp_directory_path() / ("atx_spy_disp_pnl_" + std::string(name));
  fs::remove_all(p);
  return p;
}

const std::vector<std::string> kNames = {"AAPL", "MSFT", "GOOGL", "AMZN", "NVDA", "META", "TSLA"};
const std::string kIndexSym = "SPY";
constexpr double kBaseSpot[] = {195.0, 410.0, 175.0, 185.0, 120.0, 480.0, 250.0};
constexpr double kVolBump[] = {0.00, 0.01, 0.02, 0.03, 0.04, 0.05, 0.06};
constexpr double kIndexSpot = 560.0;
constexpr int kNumDates = 12;

// A fresh SurfaceDb: kNumDates daily partitions (2026-03-01..2026-03-12), each
// holding all 7 names + SPY (uids 1..8), gentle per-date spot drift so PnL and
// the delta-hedge are non-degenerate. Returns the db root path.
[[nodiscard]] fs::path build_fixture_db(std::string_view tag) {
  const fs::path root = test_root(tag);
  auto db = SurfaceDb::create(root.string());
  EXPECT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());
  const std::int64_t base_ts = 1'700'000'000'000'000'000LL;
  for (int d = 0; d < kNumDates; ++d) {
    char date[11];
    std::snprintf(date, sizeof date, "2026-03-%02d", d + 1);
    const std::int64_t ts = base_ts + static_cast<std::int64_t>(d) * kDayNs;
    std::vector<PricedSurface> surfaces;
    surfaces.reserve(kNames.size() + 1);
    for (std::size_t i = 0; i < kNames.size(); ++i) {
      const double spot = kBaseSpot[i] * (1.0 + 0.004 * static_cast<double>(d));
      surfaces.push_back(make_surface(spot, ts, kVolBump[i], static_cast<std::uint32_t>(i + 1)));
    }
    surfaces.push_back(make_surface(kIndexSpot * (1.0 + 0.003 * static_cast<double>(d)), ts, 0.0,
                                    static_cast<std::uint32_t>(kNames.size() + 1)));
    std::vector<SurfaceArchiveItem> items;
    items.reserve(surfaces.size());
    for (std::size_t i = 0; i < kNames.size(); ++i) {
      items.push_back(SurfaceArchiveItem{kNames[i], &surfaces[i]});
    }
    items.push_back(SurfaceArchiveItem{kIndexSym, &surfaces.back()});
    EXPECT_TRUE(db->write_partition(date, items).has_value());
  }
  return root;
}

// Same fixture db, but OMITTING day index `skip_day` (0-based) — an F-c
// fit-dropped session that leaves no partition (a silent mid-range hole).
[[nodiscard]] fs::path build_fixture_db_skip(std::string_view tag, int skip_day) {
  const fs::path root = test_root(tag);
  auto db = SurfaceDb::create(root.string());
  EXPECT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());
  const std::int64_t base_ts = 1'700'000'000'000'000'000LL;
  for (int d = 0; d < kNumDates; ++d) {
    if (d == skip_day) {
      continue; // the dropped session — no partition written
    }
    char date[11];
    std::snprintf(date, sizeof date, "2026-03-%02d", d + 1);
    const std::int64_t ts = base_ts + static_cast<std::int64_t>(d) * kDayNs;
    std::vector<PricedSurface> surfaces;
    surfaces.reserve(kNames.size() + 1);
    for (std::size_t i = 0; i < kNames.size(); ++i) {
      const double spot = kBaseSpot[i] * (1.0 + 0.004 * static_cast<double>(d));
      surfaces.push_back(make_surface(spot, ts, kVolBump[i], static_cast<std::uint32_t>(i + 1)));
    }
    surfaces.push_back(make_surface(kIndexSpot * (1.0 + 0.003 * static_cast<double>(d)), ts, 0.0,
                                    static_cast<std::uint32_t>(kNames.size() + 1)));
    std::vector<SurfaceArchiveItem> items;
    items.reserve(surfaces.size());
    for (std::size_t i = 0; i < kNames.size(); ++i) {
      items.push_back(SurfaceArchiveItem{kNames[i], &surfaces[i]});
    }
    items.push_back(SurfaceArchiveItem{kIndexSym, &surfaces.back()});
    EXPECT_TRUE(db->write_partition(date, items).has_value());
  }
  return root;
}

// The D4 vega-flat dispersion config: 40Δ strangles, daily entry, held to
// expiry, daily delta-hedge. tenor_days=6 over a daily clock => a cohort
// entered on date d expires EXACTLY on date d+6.
[[nodiscard]] DispersionStrangleConfig d4_cfg(bool hold_to_expiry, bool hedge) {
  DispersionStrangleConfig cfg;
  cfg.names = kNames;
  cfg.index_symbol = kIndexSym;
  cfg.target_abs_delta = 0.40;
  cfg.tenor_days = 6.0;
  cfg.close_dte_days = 2.0; // used only by the CloseAtHorizon control
  cfg.theta_per_name_daily = 10.0;
  cfg.hold_to_expiry = hold_to_expiry;
  cfg.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, 4};
  if (hedge) {
    cfg.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0};
  }
  return cfg;
}

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

void expect_result_bit_identical(const BacktestResult &a, const BacktestResult &b) {
  ASSERT_EQ(a.size(), b.size());
  const std::array<std::pair<const std::vector<double> *, const std::vector<double> *>, 10> cols = {{
      {&a.pnl_total, &b.pnl_total},
      {&a.pnl_theta, &b.pnl_theta},
      {&a.pnl_vega, &b.pnl_vega},
      {&a.nav, &b.nav},
      {&a.gross_vega, &b.gross_vega},
      {&a.gross_delta, &b.gross_delta},
      {&a.pnl_shares, &b.pnl_shares},
      {&a.pnl_settlement, &b.pnl_settlement},
      {&a.cash, &b.cash},
      {&a.n_open_lots, &b.n_open_lots},
  }};
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a.date[i], b.date[i]) << i;
    for (const auto &[va, vb] : cols) {
      EXPECT_TRUE(bits_equal((*va)[i], (*vb)[i])) << i;
    }
  }
}

} // namespace

// ── 1. Spec shape: hold_to_expiry -> HoldToExpiry, hedge -> DeltaToZero daily ─
TEST(SpyDispersionPnl, SpecShape_HoldToExpiry_Hedged) {
  auto spec = make_dispersion_strangle_spec(d4_cfg(/*hold_to_expiry=*/true, /*hedge=*/true));
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();
  EXPECT_EQ(spec->lifecycle.holding, LifecycleSpec::Holding::HoldToExpiry);
  EXPECT_EQ(spec->lifecycle.entry, LifecycleSpec::Entry::EveryStep);
  EXPECT_EQ(spec->hedge.kind, HedgeSpec::Kind::DeltaToZero);
  EXPECT_EQ(spec->hedge.cadence, HedgeSpec::Cadence::Daily);
  EXPECT_EQ(spec->constraint.kind, CrossLegConstraint::Kind::FlatVega);
  EXPECT_EQ(spec->constraint.group_a, "basket");
  EXPECT_EQ(spec->constraint.group_b, "index");
  ASSERT_EQ(spec->legs.size(), kNames.size() + 1);
  EXPECT_EQ(spec->legs.back().group, "index");
  EXPECT_DOUBLE_EQ(spec->legs.back().size.sign, -1.0);

  // Default (hold_to_expiry=false) is unchanged: CloseAtHorizon.
  auto close = make_dispersion_strangle_spec(d4_cfg(/*hold_to_expiry=*/false, /*hedge=*/false));
  ASSERT_TRUE(close.has_value()) << close.error().to_string();
  EXPECT_EQ(close->lifecycle.holding, LifecycleSpec::Holding::CloseAtHorizon);
}

// ── 2. Every leg resolved off the FIRST db snapshot reprices to |delta| ~ 0.40 ─
TEST(SpyDispersionPnl, FortyDeltaReprice) {
  const fs::path db_root = build_fixture_db("forty_delta");
  auto db = SurfaceDb::open(db_root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  auto spec = make_dispersion_strangle_spec(d4_cfg(true, true));
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();

  auto snap = MarketSnapshot::load(clock->refs()[0].archive_path);
  ASSERT_TRUE(snap.has_value()) << snap.error().to_string();
  auto legs = resolve_spec_with_policy(*snap, *spec, nullptr);
  ASSERT_TRUE(legs.has_value()) << legs.error().to_string();
  ASSERT_EQ(legs->size(), 16u); // 8 symbols x {call, put}

  for (const auto &sl : *legs) {
    const SurfaceRef surf = snap->find(sl.leg.uid);
    ASSERT_NE(surf, nullptr);
    auto d = surf->delta(sl.leg.K, sl.leg.T, sl.leg.side);
    ASSERT_TRUE(d.has_value());
    EXPECT_NEAR(std::abs(*d), 0.40, 1e-3);
    const double F = surf->forward_at(sl.leg.T);
    if (sl.leg.side == Side::Call) {
      EXPECT_GT(sl.leg.K, F);
    } else {
      EXPECT_LT(sl.leg.K, F);
    }
  }
  std::error_code ec;
  fs::remove_all(db_root, ec);
}

// ── 3. Net entry-cohort vega ~ 0 on every date (vega-flat at entry) ──────────
TEST(SpyDispersionPnl, VegaFlatAtEntry) {
  const fs::path db_root = build_fixture_db("vega_flat");
  auto db = SurfaceDb::open(db_root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  auto spec = make_dispersion_strangle_spec(d4_cfg(true, true));
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();

  for (const SnapshotRef &ref : clock->refs()) {
    auto snap = MarketSnapshot::load(ref.archive_path);
    ASSERT_TRUE(snap.has_value()) << ref.date;
    auto legs = resolve_spec_with_policy(*snap, *spec, nullptr);
    ASSERT_TRUE(legs.has_value()) << ref.date << ": " << legs.error().to_string();
    double net_vega = 0.0;
    double gross_vega = 0.0;
    for (const auto &sl : *legs) {
      net_vega += sl.qty * sl.leg.vega * sl.multiplier;
      gross_vega += std::abs(sl.qty * sl.leg.vega * sl.multiplier);
    }
    ASSERT_GT(gross_vega, 0.0) << ref.date;
    EXPECT_LE(std::abs(net_vega), 1e-9 * gross_vega) << ref.date;
  }
  std::error_code ec;
  fs::remove_all(db_root, ec);
}

// ── 4. Held to expiry: cohorts settle at intrinsic on their aligned expiry ──
TEST(SpyDispersionPnl, HeldToExpirySettles) {
  const fs::path db_root = build_fixture_db("held_to_expiry");
  auto db = SurfaceDb::open(db_root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // Held to expiry (tenor 6 over a daily clock): cohort d expires on date d+6, so
  // the first settlement is at date index 6.
  auto spec = make_dispersion_strangle_spec(d4_cfg(/*hold_to_expiry=*/true, /*hedge=*/false));
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();
  DeclarativeStrategy strat(*spec);
  auto r = run_backtest(*clock, strat, RunConfig{});
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  ASSERT_EQ(r->size(), static_cast<std::size_t>(kNumDates));

  // Cohorts accumulate 16 lots/day with NO early close until the first expiry:
  // n_open_lots ramps 16,32,...,96 through date index 5.
  for (int i = 0; i <= 5; ++i) {
    EXPECT_DOUBLE_EQ(r->n_open_lots[i], 16.0 * static_cast<double>(i + 1)) << i;
    EXPECT_EQ(r->pnl_settlement[i], 0.0) << "no expiry before date 6, row " << i;
  }
  // At date index 6 the first (date-0) cohort reaches expiry and settles at
  // intrinsic: a non-zero settlement PnL is recorded.
  double settle_abs = 0.0;
  for (int i = 6; i < kNumDates; ++i) {
    settle_abs += std::abs(r->pnl_settlement[i]);
  }
  EXPECT_GT(settle_abs, 0.0) << "held-to-expiry cohorts must settle at intrinsic";

  // Control: the CloseAtHorizon variant closes cohorts at marks (roll-close),
  // NEVER through engine settlement -> pnl_settlement == 0 on every row. This is
  // what distinguishes "held to expiry" from "closed at horizon".
  auto close_spec = make_dispersion_strangle_spec(d4_cfg(/*hold_to_expiry=*/false, /*hedge=*/false));
  ASSERT_TRUE(close_spec.has_value()) << close_spec.error().to_string();
  DeclarativeStrategy close_strat(*close_spec);
  auto rc = run_backtest(*clock, close_strat, RunConfig{});
  ASSERT_TRUE(rc.has_value()) << rc.error().to_string();
  for (std::size_t i = 0; i < rc->size(); ++i) {
    EXPECT_EQ(rc->pnl_settlement[i], 0.0) << "CloseAtHorizon never settles, row " << i;
  }
  std::error_code ec;
  fs::remove_all(db_root, ec);
}

// ── 5. Daily delta-hedge: DeltaToZero band 0 bands post-hedge net delta ~ 0 ──
TEST(SpyDispersionPnl, DailyDeltaHedgeBandsNetDelta) {
  const fs::path db_root = build_fixture_db("delta_hedge");
  auto db = SurfaceDb::open(db_root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  auto hedged_spec = make_dispersion_strangle_spec(d4_cfg(/*hold_to_expiry=*/true, /*hedge=*/true));
  ASSERT_TRUE(hedged_spec.has_value()) << hedged_spec.error().to_string();
  DeclarativeStrategy hedged_strat(*hedged_spec);
  auto rh = run_backtest(*clock, hedged_strat, RunConfig{});
  ASSERT_TRUE(rh.has_value()) << rh.error().to_string();

  auto unhedged_spec = make_dispersion_strangle_spec(d4_cfg(/*hold_to_expiry=*/true, /*hedge=*/false));
  ASSERT_TRUE(unhedged_spec.has_value()) << unhedged_spec.error().to_string();
  DeclarativeStrategy unhedged_strat(*unhedged_spec);
  auto ru = run_backtest(*clock, unhedged_strat, RunConfig{});
  ASSERT_TRUE(ru.has_value()) << ru.error().to_string();

  // gross_delta is the POST-hedge net book delta (option + shares). Band 0 pins
  // it to ~0 every row, while the unhedged control carries a material exposure.
  double worst_hedged = 0.0;
  double worst_unhedged = 0.0;
  for (std::size_t i = 0; i < rh->size(); ++i) {
    worst_hedged = std::max(worst_hedged, std::abs(rh->gross_delta[i]));
    worst_unhedged = std::max(worst_unhedged, std::abs(ru->gross_delta[i]));
    EXPECT_LE(std::abs(rh->gross_delta[i]), 1e-6) << "post-hedge net delta not banded, row " << i;
  }
  EXPECT_GT(worst_unhedged, 1.0) << "control must carry real delta exposure to prove the hedge";
  std::printf("[spy_disp_pnl] hedge: worst |net delta| hedged=%.3e unhedged=%.3e\n", worst_hedged,
              worst_unhedged);
  std::error_code ec;
  fs::remove_all(db_root, ec);
}

// ── 6. Economic sanity: tearsheet attribution closure identity holds ────────
TEST(SpyDispersionPnl, AttributionClosureIdentity) {
  const fs::path db_root = build_fixture_db("closure");
  auto db = SurfaceDb::open(db_root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  auto spec = make_dispersion_strangle_spec(d4_cfg(/*hold_to_expiry=*/true, /*hedge=*/true));
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();
  DeclarativeStrategy strat(*spec);
  auto r = run_backtest(*clock, strat, RunConfig{});
  ASSERT_TRUE(r.has_value()) << r.error().to_string();

  const TearSheet ts = tearsheet(*r);
  const double closure = ts.attr_delta + ts.attr_gamma + ts.attr_vega + ts.attr_vanna +
                         ts.attr_volga + ts.attr_theta + ts.attr_rho + ts.attr_charm +
                         ts.attr_unexplained + ts.attr_settlement + ts.attr_shares +
                         ts.attr_financing - ts.attr_cost;
  // total_return == Σ attribution axes (settlement/shares/financing/cost incl.).
  const double scale = std::max(1.0, std::abs(ts.total_return));
  EXPECT_NEAR(ts.total_return, closure, 1e-6 * scale);
  std::error_code ec;
  fs::remove_all(db_root, ec);
}

// ── 7. Determinism: 2-run + 1-vs-4-thread bit-identity over the full run ────
TEST(SpyDispersionPnl, Determinism_TwoRunAndThreads) {
  const fs::path db_root = build_fixture_db("determinism");
  auto db = SurfaceDb::open(db_root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  auto spec = make_dispersion_strangle_spec(d4_cfg(/*hold_to_expiry=*/true, /*hedge=*/true));
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();

  const auto run = [&](unsigned n_threads) {
    DeclarativeStrategy strat(*spec);
    RunConfig cfg;
    cfg.price.n_threads = n_threads;
    return run_backtest(*clock, strat, cfg);
  };
  auto a = run(1);
  auto b = run(1); // determinism (same config, second run)
  auto c = run(4); // thread-invariance
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  ASSERT_TRUE(c.has_value()) << c.error().to_string();
  expect_result_bit_identical(*a, *b);
  expect_result_bit_identical(*a, *c);
  std::error_code ec;
  fs::remove_all(db_root, ec);
}

// ── 8. Calendar-gap audit (I1): a fit-dropped mid-range session is detectable ─
// An F-c-dropped session leaves NO partition, so the clock built from the db
// silently omits that date (a hole in the PnL track). The driver's audit
// compares an expected trading calendar (what --expected-sessions supplies)
// against the actual clock refs; the set-difference must surface exactly the
// dropped session. A full-coverage db yields zero missing.
TEST(SpyDispersionPnl, CalendarGapDetected) {
  constexpr int kSkip = 6; // omit 2026-03-07
  const fs::path db_root = build_fixture_db_skip("gap", kSkip);
  auto db = SurfaceDb::open(db_root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  // The dropped session leaves no partition -> the clock is one ref short and
  // that date is absent (the silent hole the audit must catch).
  ASSERT_EQ(clock->refs().size(), static_cast<std::size_t>(kNumDates - 1));

  std::set<std::string> actual;
  for (const SnapshotRef &ref : clock->refs()) {
    actual.insert(ref.date);
  }
  char skipped[11];
  std::snprintf(skipped, sizeof skipped, "2026-03-%02d", kSkip + 1);
  EXPECT_EQ(actual.count(skipped), 0u) << "dropped session should be absent from the clock";

  // Expected trading calendar = the full kNumDates sessions (as an external
  // --expected-sessions file would list). Set-difference == the dropped date.
  std::vector<std::string> expected;
  for (int d = 0; d < kNumDates; ++d) {
    char buf[11];
    std::snprintf(buf, sizeof buf, "2026-03-%02d", d + 1);
    expected.emplace_back(buf);
  }
  std::vector<std::string> missing;
  for (const std::string &d : expected) {
    if (actual.find(d) == actual.end()) {
      missing.push_back(d);
    }
  }
  ASSERT_EQ(missing.size(), 1u);
  EXPECT_EQ(missing[0], std::string(skipped));

  // Full coverage -> zero missing (no false positive).
  const fs::path full_root = build_fixture_db("gap_full");
  auto full_db = SurfaceDb::open(full_root.string());
  ASSERT_TRUE(full_db.has_value()) << full_db.error().to_string();
  auto full_clock = Clock::from_surface_db(*full_db);
  ASSERT_TRUE(full_clock.has_value()) << full_clock.error().to_string();
  std::set<std::string> full_actual;
  for (const SnapshotRef &ref : full_clock->refs()) {
    full_actual.insert(ref.date);
  }
  std::size_t full_missing = 0;
  for (const std::string &d : expected) {
    if (full_actual.find(d) == full_actual.end()) {
      ++full_missing;
    }
  }
  EXPECT_EQ(full_missing, 0u);

  std::error_code ec;
  fs::remove_all(db_root, ec);
  fs::remove_all(full_root, ec);
}
