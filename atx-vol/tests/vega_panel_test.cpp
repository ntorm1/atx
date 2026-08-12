// vega_panel gate tests — cross-sectional ATMF-strangle vega panel: the
// delta-targeted strangle resolver, the h-day daily-rehedged hold-PnL labeler,
// and the streaming per-symbol feature/label builder.
//
//   1. ResolveStrangle_BracketsForwardHitsTargets — K_put < F < K_call, each
//      leg's realized |delta| ≈ target, structure vega == +vega_target.
//   2. ResolveStrangle_ShortSignFlips            — sign = -1 negates qty/vega.
//   3. ResolveStrangle_InvalidArgsRejected       — |delta| targets {0, 1,
//      -0.3} and sign 0 are hard errors.
//   4. HedgedPnls_UnchangedSurfaceBleedsTheta    — same surface repeated: every
//      daily pnl is theta bleed (negative for long); hold == sum of days.
//   5. HedgedPnls_VolBumpLongVegaProfits         — parallel variance lift on
//      the mark day: the long strangle profits.
//   6. HedgedPnls_RehedgeDiffersFromEntryFixed   — two-day +2%/+2% drift: the
//      series is finite, the day-1 rehedged net delta differs from the entry
//      delta, and the rehedged sum differs from the entry-fixed-hedge PnL.
//   7. HedgedPnls_ExpiredLegRejected             — a mark past expiry (and an
//      empty marks span) is an error, never a fabricated label.
//   8. Builder_HorizonRowsAndFinishDrain         — horizon+3 pushes emit
//      exactly 3 completed rows whose labels match the free-function labeler;
//      finish() drains the rest with label_valid == 0; TSV header/row parity.
//   9. Builder_ExpiredMarkSkipsLabel             — a tenor shorter than the
//      horizon expires mid-hold: the row emits with label_valid == 0, counted
//      in skipped(), labels NaN.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"       // al_fast_opts, AmericanMethod
#include "atx/vol/priced_surface.hpp" // PricedSurface, PricingContext
#include "atx/vol/surface_parity.hpp" // SliceContext
#include "atx/vol/types.hpp"          // Result, Side
#include "atx/vol/vega_panel.hpp"
#include "atx/vol/vol_curve.hpp"   // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp" // EssviParams

using namespace atx::vol;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kDayNs = 86'400'000'000'000LL;
constexpr double kYearNs = 365.25 * 86400.0 * 1.0e9;

// Synthetic eSSVI PricedSurface (flat forward == spot, genuine American premium
// via q_eff = 0.02), 7 slices T in [0.05, 1.0]. Mirrors the
// structure_panel_test.cpp make_surface pattern.
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
constexpr double kTenorT = 0.75;
constexpr double kAbsDelta = 0.30;
constexpr double kVegaTarget = 1000.0;

[[nodiscard]] VegaPanelConfig test_cfg(int horizon) {
  VegaPanelConfig cfg;
  cfg.tenor_T = kTenorT;
  cfg.target_abs_delta = kAbsDelta;
  cfg.vega_target = kVegaTarget;
  cfg.horizon_sessions = horizon;
  return cfg;
}

// Structure net delta on `mark`, each leg's tenor re-derived from its pinned
// expiry — the same quantity the daily rehedge resets to.
[[nodiscard]] double net_delta_on(const ResolvedStructure &s, const PricedSurface &mark) {
  const std::int64_t ts = mark.pricing().now_ts_ns;
  double net = 0.0;
  for (const StructureLeg &leg : s.legs) {
    const double T = static_cast<double>(leg.expiry_ts_ns - ts) / kYearNs;
    const auto d = mark.delta(leg.strike, T, leg.side);
    EXPECT_TRUE(d.has_value());
    net += d.has_value() ? leg.qty * *d : std::numeric_limits<double>::quiet_NaN();
  }
  return net;
}

} // namespace

TEST(VegaPanel, ResolveStrangle_BracketsForwardHitsTargets) {
  const auto ps = make_surface(500.0, kT0, 0.0);
  const auto rs = resolve_atmf_strangle(ps, kTenorT, kAbsDelta, kVegaTarget, +1);
  ASSERT_TRUE(rs.has_value()) << rs.error().to_string();
  ASSERT_EQ(rs->legs.size(), 2u);
  const StructureLeg &call = rs->legs[0];
  const StructureLeg &put = rs->legs[1];
  EXPECT_EQ(call.side, Side::Call);
  EXPECT_EQ(put.side, Side::Put);
  // For |delta| < 0.5 the legs bracket the forward.
  const double F = ps.forward_at(kTenorT);
  EXPECT_GT(call.strike, F);
  EXPECT_LT(put.strike, F);
  // Realized American delta of each leg hits the target (loose tolerance).
  const auto dc = ps.delta(call.strike, kTenorT, Side::Call);
  const auto dp = ps.delta(put.strike, kTenorT, Side::Put);
  ASSERT_TRUE(dc.has_value() && dp.has_value());
  EXPECT_NEAR(*dc, kAbsDelta, 0.02);
  EXPECT_NEAR(*dp, -kAbsDelta, 0.02);
  // One shared positive qty; structure vega == +vega_target.
  EXPECT_GT(call.qty, 0.0);
  EXPECT_DOUBLE_EQ(call.qty, put.qty);
  EXPECT_NEAR(rs->entry_vega, kVegaTarget, 1e-6 * kVegaTarget);
  // Legs pinned to one absolute expiry off the entry valuation ts.
  const auto expiry = kT0 + static_cast<std::int64_t>(std::llround(kTenorT * kYearNs));
  EXPECT_EQ(call.expiry_ts_ns, expiry);
  EXPECT_EQ(put.expiry_ts_ns, expiry);
  EXPECT_GT(rs->entry_value, 0.0);
  EXPECT_GT(rs->entry_gamma, 0.0);
  EXPECT_LT(rs->entry_theta, 0.0);
  // ±target deltas nearly cancel: the strangle enters close to delta-neutral.
  EXPECT_LT(std::abs(rs->entry_delta), 0.5);
  EXPECT_DOUBLE_EQ(rs->spot, 500.0);
}

TEST(VegaPanel, ResolveStrangle_ShortSignFlips) {
  const auto ps = make_surface(500.0, kT0, 0.0);
  const auto rs = resolve_atmf_strangle(ps, kTenorT, kAbsDelta, kVegaTarget, -1);
  ASSERT_TRUE(rs.has_value()) << rs.error().to_string();
  EXPECT_LT(rs->legs[0].qty, 0.0);
  EXPECT_DOUBLE_EQ(rs->legs[0].qty, rs->legs[1].qty);
  EXPECT_NEAR(rs->entry_vega, -kVegaTarget, 1e-6 * kVegaTarget);
  EXPECT_LT(rs->entry_value, 0.0);
  EXPECT_LT(rs->entry_gamma, 0.0);
  EXPECT_GT(rs->entry_theta, 0.0);
}

TEST(VegaPanel, ResolveStrangle_InvalidArgsRejected) {
  const auto ps = make_surface(500.0, kT0, 0.0);
  EXPECT_FALSE(resolve_atmf_strangle(ps, kTenorT, 0.0, kVegaTarget, +1).has_value());
  EXPECT_FALSE(resolve_atmf_strangle(ps, kTenorT, 1.0, kVegaTarget, +1).has_value());
  EXPECT_FALSE(resolve_atmf_strangle(ps, kTenorT, -0.3, kVegaTarget, +1).has_value());
  EXPECT_FALSE(resolve_atmf_strangle(ps, kTenorT, kAbsDelta, kVegaTarget, 0).has_value());
  EXPECT_FALSE(resolve_atmf_strangle(ps, -1.0, kAbsDelta, kVegaTarget, +1).has_value());
  EXPECT_FALSE(resolve_atmf_strangle(ps, kTenorT, kAbsDelta, -5.0, +1).has_value());
}

TEST(VegaPanel, HedgedPnls_UnchangedSurfaceBleedsTheta) {
  const auto entry = make_surface(500.0, kT0, 0.0);
  const auto m1 = make_surface(500.0, kT0 + kDayNs, 0.0);
  const auto m2 = make_surface(500.0, kT0 + 2 * kDayNs, 0.0);
  const auto rs = resolve_atmf_strangle(entry, kTenorT, kAbsDelta, kVegaTarget, +1);
  ASSERT_TRUE(rs.has_value()) << rs.error().to_string();
  const PricedSurface *marks[] = {&m1, &m2};
  const auto pnls = hedged_daily_pnls(*rs, marks);
  ASSERT_TRUE(pnls.has_value()) << pnls.error().to_string();
  ASSERT_EQ(pnls->size(), 2u);
  EXPECT_LT((*pnls)[0], 0.0);
  EXPECT_LT((*pnls)[1], 0.0);
  const auto hold = hedged_hold_pnl(*rs, marks);
  ASSERT_TRUE(hold.has_value()) << hold.error().to_string();
  EXPECT_NEAR(*hold, (*pnls)[0] + (*pnls)[1], 1e-9);
}

TEST(VegaPanel, HedgedPnls_VolBumpLongVegaProfits) {
  const auto entry = make_surface(500.0, kT0, 0.0);
  const auto m1 = make_surface(500.0, kT0 + kDayNs, 0.02); // parallel variance lift
  const auto rs = resolve_atmf_strangle(entry, kTenorT, kAbsDelta, kVegaTarget, +1);
  ASSERT_TRUE(rs.has_value()) << rs.error().to_string();
  const PricedSurface *marks[] = {&m1};
  const auto pnls = hedged_daily_pnls(*rs, marks);
  ASSERT_TRUE(pnls.has_value()) << pnls.error().to_string();
  ASSERT_EQ(pnls->size(), 1u);
  EXPECT_GT((*pnls)[0], 0.0);
}

TEST(VegaPanel, HedgedPnls_RehedgeDiffersFromEntryFixed) {
  const auto entry = make_surface(500.0, kT0, 0.0);
  const auto m1 = make_surface(510.0, kT0 + kDayNs, 0.0);     // +2%
  const auto m2 = make_surface(520.2, kT0 + 2 * kDayNs, 0.0); // +2% again
  const auto rs = resolve_atmf_strangle(entry, kTenorT, kAbsDelta, kVegaTarget, +1);
  ASSERT_TRUE(rs.has_value()) << rs.error().to_string();
  const PricedSurface *marks[] = {&m1, &m2};
  const auto pnls = hedged_daily_pnls(*rs, marks);
  ASSERT_TRUE(pnls.has_value()) << pnls.error().to_string();
  ASSERT_EQ(pnls->size(), 2u);
  EXPECT_TRUE(std::isfinite((*pnls)[0]));
  EXPECT_TRUE(std::isfinite((*pnls)[1]));
  // The rehedge is real: after a +2% drift the structure's net delta on the
  // day-1 surface (long gamma) has moved away from the entry hedge ratio.
  const double d1 = net_delta_on(*rs, m1);
  EXPECT_TRUE(std::isfinite(d1));
  EXPECT_GT(std::abs(d1 - rs->entry_delta), 1e-6);
  // And the daily-rehedged hold differs from the entry-fixed hedge PnL.
  double value2 = 0.0;
  for (const StructureLeg &leg : rs->legs) {
    const double T = static_cast<double>(leg.expiry_ts_ns - m2.pricing().now_ts_ns) / kYearNs;
    const auto px = m2.fair_value(leg.strike, T, leg.side);
    ASSERT_TRUE(px.has_value());
    value2 += leg.qty * *px;
  }
  const double fixed_hedge =
      value2 - rs->entry_value - rs->entry_delta * (m2.pricing().S - rs->spot);
  EXPECT_GT(std::abs(((*pnls)[0] + (*pnls)[1]) - fixed_hedge), 0.1);
}

TEST(VegaPanel, HedgedPnls_ExpiredLegRejected) {
  const auto entry = make_surface(500.0, kT0, 0.0);
  // Front tenor 0.05y ~ 18 days: a mark 60 days later is past expiry.
  const auto rs = resolve_atmf_strangle(entry, 0.05, kAbsDelta, kVegaTarget, +1);
  ASSERT_TRUE(rs.has_value()) << rs.error().to_string();
  const auto late = make_surface(500.0, kT0 + 60 * kDayNs, 0.0);
  const PricedSurface *marks[] = {&late};
  EXPECT_FALSE(hedged_daily_pnls(*rs, marks).has_value());
  EXPECT_FALSE(hedged_hold_pnl(*rs, marks).has_value());
  // Empty marks span: nothing to label.
  EXPECT_FALSE(hedged_daily_pnls(*rs, {}).has_value());
}

TEST(VegaPanel, Builder_HorizonRowsAndFinishDrain) {
  constexpr int kHorizon = 3;
  VegaPanelBuilder b(test_cfg(kHorizon), "SPY");
  std::vector<PricedSurface> surfs;
  surfs.reserve(6);
  for (int d = 0; d < 6; ++d) {
    surfs.push_back(
        make_surface(500.0 * (1.0 + 0.002 * static_cast<double>(d)), kT0 + d * kDayNs, 0.0));
  }
  std::vector<VegaPanelRow> rows;
  for (int d = 0; d < 6; ++d) {
    char key[16];
    std::snprintf(key, sizeof key, "2026-03-%02d", d + 2);
    auto r = b.push(key, surfs[static_cast<std::size_t>(d)]);
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    if (r->has_value()) {
      rows.push_back(**r);
    }
  }
  // horizon+3 pushes -> exactly 3 completed rows.
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].key, "2026-03-02");
  EXPECT_EQ(rows[0].symbol, "SPY");
  EXPECT_TRUE(rows[0].label_valid);
  EXPECT_TRUE(std::isfinite(rows[0].label_pnl_h));
  EXPECT_TRUE(std::isfinite(rows[0].label_pnl_1d));
  EXPECT_EQ(b.skipped(), 0u);
  // Entry-day features are live; warm-up windows are NaN, never fabricated.
  EXPECT_DOUBLE_EQ(rows[0].spot, 500.0);
  EXPECT_GT(rows[0].iv_1m, 0.0);
  EXPECT_GT(rows[0].iv_1y, 0.0);
  EXPECT_TRUE(std::isfinite(rows[0].term_slope_1m_1y));
  EXPECT_TRUE(std::isfinite(rows[0].fwd_vol_1m_1y));
  EXPECT_TRUE(std::isfinite(rows[0].rr25_1y));
  EXPECT_TRUE(std::isfinite(rows[0].bf25_1y));
  EXPECT_NEAR(rows[0].entry_vega, kVegaTarget, 1e-6 * kVegaTarget);
  EXPECT_GT(rows[0].entry_gamma, 0.0);
  EXPECT_LT(rows[0].entry_theta, 0.0);
  EXPECT_LT(std::abs(rows[0].entry_delta_net), 0.5);
  EXPECT_GT(rows[0].strike_call, rows[0].strike_put);
  EXPECT_TRUE(std::isnan(rows[0].rv_21));
  EXPECT_TRUE(std::isnan(rows[0].ret_21d));
  EXPECT_TRUE(std::isnan(rows[0].div_1y_21));
  EXPECT_TRUE(std::isnan(rows[0].vol_of_vol_21));
  EXPECT_TRUE(std::isnan(rows[0].iv_1y_rank_252));
  // The builder's incremental label matches the free-function labeler exactly.
  const auto rs = resolve_atmf_strangle(surfs[0], kTenorT, kAbsDelta, kVegaTarget, +1);
  ASSERT_TRUE(rs.has_value());
  const PricedSurface *marks[] = {&surfs[1], &surfs[2], &surfs[3]};
  const auto pnls = hedged_daily_pnls(*rs, marks);
  ASSERT_TRUE(pnls.has_value()) << pnls.error().to_string();
  const auto hold = hedged_hold_pnl(*rs, marks);
  ASSERT_TRUE(hold.has_value());
  EXPECT_NEAR(rows[0].label_pnl_h, *hold, 1e-9 * std::max(1.0, std::abs(*hold)));
  EXPECT_NEAR(rows[0].label_pnl_1d, (*pnls)[0], 1e-9 * std::max(1.0, std::abs((*pnls)[0])));

  // Keys must be strictly ascending.
  EXPECT_FALSE(b.push("2026-01-01", make_surface(500.0, kT0 + 99 * kDayNs, 0.0)).has_value());

  // finish() drains the 3 still-pending entries oldest-first: features intact,
  // labels NaN, label_valid 0; second call is empty.
  auto rest = b.finish();
  ASSERT_EQ(rest.size(), 3u);
  EXPECT_EQ(rest[0].key, "2026-03-05");
  for (const VegaPanelRow &r : rest) {
    EXPECT_FALSE(r.label_valid);
    EXPECT_TRUE(std::isnan(r.label_pnl_h));
    EXPECT_TRUE(std::isnan(r.label_pnl_1d));
    EXPECT_GT(r.iv_1y, 0.0);
  }
  EXPECT_TRUE(b.finish().empty());

  // TSV round-trip: header and row emit the same number of columns.
  const auto count_tabs = [](const std::string &s) {
    std::size_t n = 0;
    for (const char c : s) {
      n += (c == '\t') ? 1u : 0u;
    }
    return n;
  };
  EXPECT_EQ(count_tabs(vega_panel_tsv_header()), count_tabs(to_tsv_line(rows[0])));
  EXPECT_EQ(count_tabs(vega_panel_tsv_header()), count_tabs(to_tsv_line(rest[0])));
}

TEST(VegaPanel, Builder_ExpiredMarkSkipsLabel) {
  // Tenor ~3 calendar days with a 5-session horizon: the legs expire before
  // the label window fills, so the mark path MUST fail-soft (counted), never
  // fabricate a post-expiry mark.
  VegaPanelConfig cfg = test_cfg(5);
  cfg.tenor_T = 3.0 / 365.25;
  VegaPanelBuilder b(cfg, "SPY");
  std::optional<VegaPanelRow> first;
  for (int d = 0; d < 6; ++d) {
    char key[16];
    std::snprintf(key, sizeof key, "2026-04-%02d", d + 2);
    auto r = b.push(key, make_surface(500.0, kT0 + d * kDayNs, 0.0));
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    if (r->has_value() && !first.has_value()) {
      first = **r;
    }
  }
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->key, "2026-04-02");
  EXPECT_FALSE(first->label_valid);
  EXPECT_TRUE(std::isnan(first->label_pnl_h));
  EXPECT_TRUE(std::isnan(first->label_pnl_1d));
  EXPECT_GE(b.skipped(), 1u);
}
