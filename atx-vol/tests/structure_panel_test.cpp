// structure_panel gate tests — daily one-day-hold delta-neutral structure PnL
// and the streaming feature/label panel builder (SPY structure-selector ML).
//
//   1. ResolveStraddle_StrikeAtForwardVegaTarget — ATMF straddle: both legs at
//      K = F(T), structure vega == +vega_target, expiry pinned off now_ts.
//   2. ResolveStraddle_ShortSignFlips           — sign = -1 negates qty/greeks.
//   3. Pnl_UnchangedSurfaceBleedsTheta          — same surface one day later:
//      long straddle loses ~theta*dt, never gains.
//   4. Pnl_SpotGapLongGammaProfits              — +3% overnight gap, flat vols:
//      the front (gamma-heavy) straddle makes money net of one day of theta.
//   5. Pnl_VolBumpLongVegaProfits               — parallel vol lift, spot flat:
//      both vega-normalized straddles profit.
//   6. Pnl_ExpiredLegRejected                   — mark past expiry is an error,
//      never a fabricated mark.
//   7. Builder_EmitsRowPerCompletedDay          — push t0 -> no row; push t1 ->
//      completed t0 row (features + labels); non-ascending key rejected.
//   8. Builder_WindowedFeaturesFillIn           — RV/momentum NaN until their
//      windows fill, finite afterwards.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"       // al_fast_opts, AmericanMethod
#include "atx/vol/api/backtest/priced_surface.hpp" // PricedSurface, PricingContext
#include "backtest/structure_panel.hpp"
#include "atx/vol/api/fitting/surface_parity.hpp" // SliceContext
#include "atx/vol/api/core/types.hpp"          // Result, Side
#include "atx/vol/api/fitting/vol_curve.hpp"      // CurveSurface, EssviCurve
#include "atx/vol/api/fitting/vol_surface.hpp"    // EssviParams

using namespace atx::vol;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kDayNs = 86'400'000'000'000LL;
constexpr double kYearNs = 365.25 * 86400.0 * 1.0e9;

// Synthetic eSSVI PricedSurface (flat forward == spot, genuine American premium
// via q_eff = 0.02), 7 slices T in [0.05, 1.0]. Mirrors the
// surface_db_backtest_test.cpp make_surface pattern.
[[nodiscard]] PricedSurface make_surface(double S, std::int64_t now_ts, double vol_bump) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = (0.04 + 0.005 * static_cast<double>(i)) * T + vol_bump * T;
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
  pc.uid = 1;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value()) << (ps.has_value() ? std::string{} : ps.error().to_string());
  return std::move(*ps);
}

constexpr std::int64_t kT0 = 1'700'000'000'000'000'000LL;
constexpr double kFrontT = 0.10;
constexpr double kBackT = 0.75;
constexpr double kVegaTarget = 1000.0;

[[nodiscard]] StructurePanelConfig test_cfg() {
  StructurePanelConfig cfg;
  cfg.front_T = kFrontT;
  cfg.back_T = kBackT;
  cfg.vega_target = kVegaTarget;
  return cfg;
}

} // namespace

TEST(StructurePanel, ResolveStraddle_StrikeAtForwardVegaTarget) {
  const auto ps = make_surface(500.0, kT0, 0.0);
  const auto rs = resolve_atmf_straddle(ps, kFrontT, kVegaTarget, +1.0);
  ASSERT_TRUE(rs.has_value()) << rs.error().to_string();
  ASSERT_EQ(rs->legs.size(), 2u);
  const double F = ps.forward_at(kFrontT);
  EXPECT_NEAR(rs->legs[0].strike, F, 1e-9 * F);
  EXPECT_NEAR(rs->legs[1].strike, F, 1e-9 * F);
  EXPECT_NE(rs->legs[0].side, rs->legs[1].side);
  // Both legs share one expiry, pinned off the entry surface's valuation ts.
  const auto expiry = kT0 + static_cast<std::int64_t>(kFrontT * kYearNs);
  EXPECT_EQ(rs->legs[0].expiry_ts_ns, expiry);
  EXPECT_EQ(rs->legs[1].expiry_ts_ns, expiry);
  // Same qty per leg, positive for a long structure; vega hits the target.
  EXPECT_GT(rs->legs[0].qty, 0.0);
  EXPECT_DOUBLE_EQ(rs->legs[0].qty, rs->legs[1].qty);
  EXPECT_NEAR(rs->entry_vega, kVegaTarget, 1e-6 * kVegaTarget);
  EXPECT_GT(rs->entry_value, 0.0);
  EXPECT_GT(rs->entry_gamma, 0.0);
  EXPECT_LT(rs->entry_theta, 0.0);
  EXPECT_DOUBLE_EQ(rs->spot, 500.0);
}

TEST(StructurePanel, ResolveStraddle_ShortSignFlips) {
  const auto ps = make_surface(500.0, kT0, 0.0);
  const auto rs = resolve_atmf_straddle(ps, kFrontT, kVegaTarget, -1.0);
  ASSERT_TRUE(rs.has_value()) << rs.error().to_string();
  EXPECT_LT(rs->legs[0].qty, 0.0);
  EXPECT_NEAR(rs->entry_vega, -kVegaTarget, 1e-6 * kVegaTarget);
  EXPECT_LT(rs->entry_value, 0.0);
  EXPECT_LT(rs->entry_gamma, 0.0);
  EXPECT_GT(rs->entry_theta, 0.0);
}

TEST(StructurePanel, Pnl_UnchangedSurfaceBleedsTheta) {
  const auto entry = make_surface(500.0, kT0, 0.0);
  const auto mark = make_surface(500.0, kT0 + kDayNs, 0.0);
  const auto rs = resolve_atmf_straddle(entry, kFrontT, kVegaTarget, +1.0);
  ASSERT_TRUE(rs.has_value());
  const auto pnl = delta_neutral_pnl(*rs, mark);
  ASSERT_TRUE(pnl.has_value()) << pnl.error().to_string();
  EXPECT_LT(*pnl, 0.0);
  // Bounded by theta: the decay realized over dt should be the annualized
  // entry theta scaled to one day, within a loose discrete-vs-PDE tolerance.
  const double dt = static_cast<double>(kDayNs) / kYearNs;
  const double theo = rs->entry_theta * dt;
  EXPECT_GT(*pnl, 3.0 * theo); // not wildly more negative than theta predicts
}

TEST(StructurePanel, Pnl_SpotGapLongGammaProfits) {
  const auto entry = make_surface(500.0, kT0, 0.0);
  const auto mark = make_surface(515.0, kT0 + kDayNs, 0.0); // +3% overnight gap
  const auto rs = resolve_atmf_straddle(entry, kFrontT, kVegaTarget, +1.0);
  ASSERT_TRUE(rs.has_value());
  const auto pnl = delta_neutral_pnl(*rs, mark);
  ASSERT_TRUE(pnl.has_value()) << pnl.error().to_string();
  EXPECT_GT(*pnl, 0.0); // gamma gain on a 3-sigma-ish gap dwarfs one day of theta
}

TEST(StructurePanel, Pnl_VolBumpLongVegaProfits) {
  const auto entry = make_surface(500.0, kT0, 0.0);
  const auto mark = make_surface(500.0, kT0 + kDayNs, 0.02); // parallel variance lift
  const auto front = resolve_atmf_straddle(entry, kFrontT, kVegaTarget, +1.0);
  const auto back = resolve_atmf_straddle(entry, kBackT, kVegaTarget, +1.0);
  ASSERT_TRUE(front.has_value());
  ASSERT_TRUE(back.has_value());
  const auto pnl_front = delta_neutral_pnl(*front, mark);
  const auto pnl_back = delta_neutral_pnl(*back, mark);
  ASSERT_TRUE(pnl_front.has_value());
  ASSERT_TRUE(pnl_back.has_value());
  EXPECT_GT(*pnl_front, 0.0);
  EXPECT_GT(*pnl_back, 0.0);
}

TEST(StructurePanel, Pnl_ExpiredLegRejected) {
  const auto entry = make_surface(500.0, kT0, 0.0);
  // Mark far beyond the front expiry (0.10y ~ 36 days): T at mark is negative.
  const auto mark = make_surface(500.0, kT0 + 60 * kDayNs, 0.0);
  const auto rs = resolve_atmf_straddle(entry, kFrontT, kVegaTarget, +1.0);
  ASSERT_TRUE(rs.has_value());
  const auto pnl = delta_neutral_pnl(*rs, mark);
  EXPECT_FALSE(pnl.has_value());
}

TEST(StructurePanel, Builder_EmitsRowPerCompletedDay) {
  StructurePanelBuilder b(test_cfg());
  const auto d0 = make_surface(500.0, kT0, 0.0);
  const auto d1 = make_surface(505.0, kT0 + kDayNs, 0.0);

  auto r0 = b.push("2026-01-02", d0);
  ASSERT_TRUE(r0.has_value()) << r0.error().to_string();
  EXPECT_FALSE(r0->has_value()); // first day: label not yet observable

  auto r1 = b.push("2026-01-05", d1);
  ASSERT_TRUE(r1.has_value()) << r1.error().to_string();
  ASSERT_TRUE(r1->has_value());
  const PanelRow &row = **r1;
  EXPECT_EQ(row.key, "2026-01-02");
  EXPECT_TRUE(row.pnl_valid);
  EXPECT_TRUE(std::isfinite(row.pnl_front));
  EXPECT_TRUE(std::isfinite(row.pnl_back));
  EXPECT_DOUBLE_EQ(row.spot, 500.0);
  EXPECT_GT(row.iv_1w, 0.0);
  EXPECT_GT(row.iv_1m, 0.0);
  EXPECT_GT(row.iv_1y, 0.0);
  EXPECT_TRUE(std::isfinite(row.short_slope));
  EXPECT_TRUE(std::isfinite(row.term_slope));
  EXPECT_TRUE(std::isfinite(row.fwd_vol_front_back));
  // Model-free strip vols on the smooth synthetic surface: finite, near ATM.
  EXPECT_GT(row.vsw_1m, 0.0);
  EXPECT_GT(row.vsw_1y, 0.0);
  EXPECT_LT(std::abs(row.vsw_conv_1m), 0.25);
  EXPECT_GT(row.front_gamma, 0.0);
  EXPECT_LT(row.front_theta, 0.0);
  EXPECT_TRUE(std::isfinite(row.front_delta));
  EXPECT_TRUE(std::isfinite(row.front_vanna));
  EXPECT_TRUE(std::isfinite(row.front_volga));
  EXPECT_TRUE(std::isfinite(row.back_delta));
  // One-day-old history: momentum/RV windows are not yet observable.
  EXPECT_TRUE(std::isnan(row.ret_1d));
  EXPECT_TRUE(std::isnan(row.rv5));

  // Keys must be strictly ascending.
  auto bad = b.push("2026-01-04", make_surface(506.0, kT0 + 2 * kDayNs, 0.0));
  EXPECT_FALSE(bad.has_value());

  // finish() surrenders the pending final session: features intact, labels
  // NaN, pnl_valid false; second call is empty.
  auto last = b.finish();
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->key, "2026-01-05");
  EXPECT_FALSE(last->pnl_valid);
  EXPECT_TRUE(std::isnan(last->pnl_front));
  EXPECT_TRUE(std::isnan(last->pnl_back));
  EXPECT_GT(last->iv_1m, 0.0);
  EXPECT_FALSE(b.finish().has_value());
}

TEST(StructurePanel, Builder_WindowedFeaturesFillIn) {
  StructurePanelBuilder b(test_cfg());
  std::vector<PanelRow> rows;
  for (int d = 0; d < 8; ++d) {
    const double S = 500.0 * (1.0 + 0.002 * static_cast<double>(d));
    char key[16];
    std::snprintf(key, sizeof key, "2026-02-%02d", d + 2);
    auto r = b.push(key, make_surface(S, kT0 + d * kDayNs, 0.0));
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    if (r->has_value()) {
      rows.push_back(**r);
    }
  }
  ASSERT_EQ(rows.size(), 7u);
  // Day 1 (second row's entry day) has one prior close: ret_1d becomes finite.
  EXPECT_TRUE(std::isnan(rows[0].ret_1d));
  EXPECT_TRUE(std::isfinite(rows[1].ret_1d));
  EXPECT_NEAR(rows[1].ret_1d, std::log(rows[1].spot / rows[0].spot), 1e-12);
  // RV5 needs 6 closes: first finite on the 6th entry day (row index 5).
  EXPECT_TRUE(std::isnan(rows[4].rv5));
  EXPECT_TRUE(std::isfinite(rows[5].rv5));
  EXPECT_GT(rows[5].rv5, 0.0);
  // IV momentum mirrors spot momentum availability.
  EXPECT_TRUE(std::isnan(rows[0].div_1m_1d));
  EXPECT_TRUE(std::isfinite(rows[1].div_1m_1d));
  // 21-day windows stay NaN over an 8-day corpus.
  EXPECT_TRUE(std::isnan(rows[6].rv21));
  EXPECT_TRUE(std::isnan(rows[6].vol_of_vol_21));
  // TSV round-trip: header and row emit the same number of columns.
  const std::string header = panel_tsv_header();
  const std::string line = to_tsv_line(rows[6]);
  const auto count_tabs = [](const std::string &s) {
    std::size_t n = 0;
    for (const char c : s) {
      n += (c == '\t') ? 1u : 0u;
    }
    return n;
  };
  EXPECT_EQ(count_tabs(header), count_tabs(line));
}
