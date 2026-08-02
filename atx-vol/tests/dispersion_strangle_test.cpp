// dispersion_strangle.hpp gate tests.
//
// Exercises `make_dispersion_strangle_spec`, the pure config -> `StrategySpec`
// builder for the dispersion-strangle strategy (long equal-theta single-name
// strangles vs a short vega-flat index strangle):
//
//   1. SpecShape                        — leg/group/size/constraint/lifecycle
//                                          wiring matches the doc-comment contract.
//   2. RejectsBadConfig                 — every InvalidArgument validation rule.
//   3. EntryMath_EqualTheta_VegaFlat_FortyDelta
//                                        — resolved against a real snapshot: 40d
//                                          strikes reprice, per-name theta equal,
//                                          cohort net vega ~ 0, index qty < 0.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"          // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"          // MarketSnapshot
#include "atx/vol/dispersion.hpp"        // MissingNamePolicy, MissingNameSpec
#include "atx/vol/dispersion_strangle.hpp"
#include "atx/vol/priced_surface.hpp"    // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"          // StrategySpec, resolve_spec_with_policy
#include "atx/vol/surface_archive.hpp"   // write_surface_archive_v2_file, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"    // SliceContext
#include "atx/vol/types.hpp"             // Side, Result, ErrorCode
#include "atx/vol/vol_curve.hpp"         // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"       // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;

// A synthetic eSSVI PricedSurface (flat forward, genuine American premium via
// q_eff=0.02), slices T in [0.05, 1.0]. Mirrors strategy_test.cpp's make_surface.
[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double S, double fwd,
                                         std::int64_t now_ts, double vol_bump = 0.0) {
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
    e.F = fwd;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, fwd, 0.0, 0.02, 250, 7});
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

[[nodiscard]] fs::path fresh_dir() {
  const fs::path dir = fs::temp_directory_path() / "atx-dispersion-strangle";
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

// Write `items` (symbol -> surface) as one date's archive; return its path.
[[nodiscard]] std::string
write_archive(const fs::path &dir, const std::string &date,
              const std::vector<std::pair<std::string, const PricedSurface *>> &items) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  std::vector<SurfaceArchiveItem> its;
  its.reserve(items.size());
  for (const auto &[sym, ps] : items) {
    its.push_back(SurfaceArchiveItem{sym, ps});
  }
  const Status st = write_surface_archive_v2_file(path, its);
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  return path;
}

// One archive holding 4 surfaces: 3 "names" (AAA/BBB/CCC) + index SPX, per the
// task-3 brief's fixture spec.
[[nodiscard]] Result<MarketSnapshot> load_fixture_snapshot() {
  const PricedSurface aaa = make_surface(1, 100.0, 100.0, kBaseNow, 0.00);
  const PricedSurface bbb = make_surface(2, 150.0, 150.0, kBaseNow, 0.06);
  const PricedSurface ccc = make_surface(3, 200.0, 200.0, kBaseNow, 0.12);
  const PricedSurface spx = make_surface(9, 500.0, 500.0, kBaseNow, 0.02);
  const fs::path dir = fresh_dir();
  const std::string path = write_archive(dir, "2026-12-01",
                                         {{"AAA", &aaa}, {"BBB", &bbb}, {"CCC", &ccc}, {"SPX", &spx}});
  return MarketSnapshot::load(path);
}

} // namespace

// ── 1. Spec shape: leg/group/size/constraint/lifecycle wiring ──────────────
TEST(DispersionStrangle, SpecShape) {
  DispersionStrangleConfig cfg;
  cfg.names = {"AAA", "BBB", "CCC"};
  cfg.index_symbol = "SPX";
  cfg.missing = {MissingNamePolicy::DropRenormalize, 2};
  auto spec = make_dispersion_strangle_spec(cfg);
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();
  ASSERT_EQ(spec->legs.size(), 4u);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(spec->legs[i].group, "basket");
    EXPECT_EQ(spec->legs[i].size.kind, SizeSpec::Kind::TargetTheta);
    EXPECT_DOUBLE_EQ(spec->legs[i].size.sign, +1.0);
    EXPECT_EQ(spec->legs[i].structure.kind, StructureSpec::Kind::Strangle);
    EXPECT_DOUBLE_EQ(spec->legs[i].tenor.target_T, 90.0 / 365.25);
  }
  EXPECT_EQ(spec->legs[3].symbol, "SPX");
  EXPECT_EQ(spec->legs[3].group, "index");
  EXPECT_DOUBLE_EQ(spec->legs[3].size.sign, -1.0);
  EXPECT_EQ(spec->constraint.kind, CrossLegConstraint::Kind::FlatVega);
  EXPECT_EQ(spec->constraint.group_a, "basket");
  EXPECT_EQ(spec->constraint.group_b, "index");
  EXPECT_EQ(spec->lifecycle.holding, LifecycleSpec::Holding::CloseAtHorizon);
  EXPECT_DOUBLE_EQ(spec->lifecycle.roll_at_T, 10.0 / 365.25);
  EXPECT_EQ(spec->lifecycle.entry, LifecycleSpec::Entry::EveryStep);
}

// ── 2. Every InvalidArgument validation rule ────────────────────────────────
TEST(DispersionStrangle, RejectsBadConfig) {
  DispersionStrangleConfig ok;
  ok.names = {"AAA"};
  ok.missing.min_names = 1;
  ASSERT_TRUE(make_dispersion_strangle_spec(ok).has_value())
      << make_dispersion_strangle_spec(ok).error().to_string();
  auto expect_reject = [&](auto mutate) {
    DispersionStrangleConfig c = ok;
    mutate(c);
    auto r = make_dispersion_strangle_spec(c);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  };
  expect_reject([](auto &c) { c.names.clear(); });
  expect_reject([](auto &c) { c.index_symbol = "AAA"; });
  expect_reject([](auto &c) { c.index_symbol = ""; });
  // Case-insensitive index/name collision: names={"AAA"}, index="aaa" must
  // resolve to the same canonical symbol as an exact-case collision.
  expect_reject([](auto &c) { c.index_symbol = "aaa"; });
  // Duplicate name under the same canonicalization.
  expect_reject([](auto &c) { c.names = {"AAA", "aaa"}; });
  expect_reject([](auto &c) { c.target_abs_delta = 1.0; });
  expect_reject([](auto &c) { c.target_abs_delta = 0.0; });
  expect_reject([](auto &c) { c.tenor_days = 10.0; c.close_dte_days = 10.0; });
  expect_reject([](auto &c) { c.close_dte_days = -1.0; });
  expect_reject([](auto &c) { c.theta_per_name_daily = 0.0; });
  expect_reject([](auto &c) { c.index_base_vega = 0.0; });
  expect_reject([](auto &c) { c.entry_every_n_days = 0; });
  expect_reject([](auto &c) { c.missing.min_names = 0; });
  expect_reject([](auto &c) { c.missing.min_names = 5; });
}

// ── 3. Entry math: 40d strikes reprice, equal theta, vega-flat, short index ─
TEST(DispersionStrangle, EntryMath_EqualTheta_VegaFlat_FortyDelta) {
  auto snap = load_fixture_snapshot();
  ASSERT_TRUE(snap.has_value()) << snap.error().to_string();
  DispersionStrangleConfig cfg;
  cfg.names = {"AAA", "BBB", "CCC"};
  cfg.index_symbol = "SPX";
  cfg.tenor_days = 90.0;
  cfg.theta_per_name_daily = 10.0;
  cfg.missing = {MissingNamePolicy::DropRenormalize, 2};
  auto spec = make_dispersion_strangle_spec(cfg);
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();
  auto legs = resolve_spec_with_policy(*snap, *spec, nullptr);
  ASSERT_TRUE(legs.has_value()) << legs.error().to_string();
  ASSERT_EQ(legs->size(), 8u); // 4 symbols x {call, put}

  // 40-delta strike correctness: every resolved leg reprices to |delta| ~ 0.40
  // (mirror spy_strangle_backtest_test::FortyDeltaEntry: reprice via
  // surf->delta(K, T, side), tolerance 1e-3; call K above forward, put below).
  for (const auto &sl : *legs) {
    const SurfaceRef surf = snap->find(sl.leg.uid);
    ASSERT_NE(surf, nullptr);
    auto d = surf->delta(sl.leg.K, sl.leg.T, sl.leg.side);
    ASSERT_TRUE(d.has_value());
    EXPECT_NEAR(std::abs(*d), 0.40, 1e-3);
    const double F = surf->forward_at(sl.leg.T);
    if (sl.leg.side == Side::Call) EXPECT_GT(sl.leg.K, F); else EXPECT_LT(sl.leg.K, F);
  }

  // Equal theta: each name's |sum(qty*theta*mult)| == 10 $/day * 365.25, all
  // names equal within 1e-6 relative.
  const double want_theta = 10.0 * 365.25;
  std::map<std::uint32_t, double> theta_by_uid;
  double net_vega = 0.0, gross_vega = 0.0;
  for (const auto &sl : *legs) {
    if (sl.leg.group == "basket") theta_by_uid[sl.leg.uid] += sl.qty * sl.leg.theta * sl.multiplier;
    net_vega += sl.qty * sl.leg.vega * sl.multiplier;
    gross_vega += std::abs(sl.qty * sl.leg.vega * sl.multiplier);
  }
  ASSERT_EQ(theta_by_uid.size(), 3u);
  for (const auto &[uid, th] : theta_by_uid) {
    EXPECT_NEAR(std::abs(th), want_theta, 1e-6 * want_theta) << uid;
  }
  // Vega-flat at entry: net cohort vega ~ 0 (FlatVega scale is exact in fp).
  EXPECT_LE(std::abs(net_vega), 1e-9 * gross_vega);
  // Short index: negative qty on index legs.
  for (const auto &sl : *legs) {
    if (sl.leg.group == "index") EXPECT_LT(sl.qty, 0.0);
  }
}
