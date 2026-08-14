#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "atx/vol/api/fitting/calib.hpp"
#include "atx/vol/api/marketdata/opra_panel.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/marketdata/universe.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"

// Real-data SOTA-accuracy acceptance on a cached SPY OPRA cbbo-1m (NBBO) slice.
//
// SPY is the index-ETF counterpart to the XOM single-name proof: ~14k contracts
// across ~35 weekly/monthly/LEAP expiries, a steep index crash-put skew, and
// penny-tight NBBO spreads. The parquet is produced offline from a cached DBN
// (opra_dbn_to_parquet) — no API spend at test time. If it is absent (a fresh
// checkout / CI without the fixture) the test SKIPS.
//
// The gate is VEGA-WEIGHTED vol RMSE over the LIQUID surface, NOT fraction-in-
// bid-ask: at penny half-spreads even a ~0.4 vol-pt fit lands outside the band,
// so in-bid-ask is a tick-size metric, not an accuracy one. Ultra-short (< ~1wk)
// and the deepest tails (|k| > 1.5) are separate regimes and are excluded.

namespace {

using atx::vol::build_observations;
using atx::vol::CalibOpts;
using atx::vol::data_install;
using atx::vol::FitPreset;
using atx::vol::load_opra_cbbo_parquet;
using atx::vol::make_session_inputs;
using atx::vol::OpraLoadSpec;
using atx::vol::SessionDiagnostics;
using atx::vol::Underlying;
using atx::vol::Universe;
using atx::vol::VolaSession;
using atx::vol::VolCurveKind;

// Locate the cached SPY parquet across the paths a test binary might run from.
[[nodiscard]] std::string find_spy_parquet() {
  const char* candidates[] = {
      "data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
      "../data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
      "../../data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
      "C:/atx/data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
  };
  for (const char* c : candidates) {
    if (std::filesystem::exists(c)) {
      return c;
    }
  }
  return {};
}

}  // namespace

TEST(SpyRealOpra, LiquidSurfaceFitsToOneVolPointVegaWeighted) {
  const std::string path = find_spy_parquet();
  if (path.empty()) {
    GTEST_SKIP() << "cached SPY OPRA parquet not found; run the databento pull + "
                    "opra_dbn_to_parquet to materialise the fixture.";
  }

  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "SPY";
  spec.snapshot_iso = "2026-06-05T19:55:00Z";
  spec.r = 0.043;
  const auto panel = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  // Sanity: SPY implied spot in a sane band for this snapshot era.
  EXPECT_GT(panel->implied_spot, 600.0);
  EXPECT_LT(panel->implied_spot, 900.0);

  Universe u;
  const auto uid = data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value());
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  const Underlying* U = *under;

  // Fast preset: the backbone fit whose vega-weighted accuracy is the headline
  // (the wing-residual layer does not move it on SPY — see session.cpp note).
  const auto in = make_session_inputs(FitPreset::Fast, panel->implied_spot, spec.r,
                                      panel->frame.snapshot_ts_ns);
  const auto sess = VolaSession::from_frame(panel->frame, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const auto ctx = sess->expiries();
  EXPECT_GT(ctx.size(), std::size_t{25});  // a full SPY board: many expiries

  // Per liquid slice (T >= ~1wk), vega^2-weighted RMS of (model iv - market iv),
  // with the market iv from build_observations' European inversion (SPY American
  // premium is tiny, so this tracks the de-Am'd smile the fit saw).
  std::vector<double> liq_vw;
  for (std::size_t i = 0; i < ctx.size() && i < U->chains.size(); ++i) {
    const double T = ctx[i].T;
    if (T < 0.019) {
      continue;  // ultra-short 0DTE/weekly regime, excluded
    }
    const double F = ctx[i].forward;
    const double df = std::exp(-spec.r * T);
    const auto obs = build_observations(U->chains[i], F, T, df, CalibOpts{});
    if (!obs.has_value() || obs->obs.size() < 5) {
      continue;
    }
    double wsum = 0.0, wsr2 = 0.0;
    for (const auto& o : obs->obs) {
      const double K = F * std::exp(o.k);
      const double res = sess->iv(K, T) - o.sigma_mkt;
      const double w = (o.vega > 0.0) ? o.vega * o.vega : 0.0;
      wsum += w;
      wsr2 += w * res * res;
    }
    if (wsum > 0.0) {
      liq_vw.push_back(std::sqrt(wsr2 / wsum));
    }
  }

  ASSERT_GT(liq_vw.size(), std::size_t{20});
  std::sort(liq_vw.begin(), liq_vw.end());
  const double median = liq_vw[liq_vw.size() / 2];
  std::size_t under_2vp = 0;
  for (const double x : liq_vw) {
    if (x < 0.02) {
      ++under_2vp;
    }
  }

  // SOTA gate: the median liquid slice fits to ~1 vol point vega-weighted, and
  // the large majority are within 2 vol points.
  EXPECT_LT(median, 0.015) << "median vega-weighted RMSE (vol) over liquid slices";
  EXPECT_GE(under_2vp * 100, liq_vw.size() * 75)  // >= 75% within 2 vol points
      << under_2vp << " of " << liq_vw.size() << " liquid slices within 2 vol pts";
}

// The dense/SVI served surface (ConvexDense override path) must now REPORT a
// measured calendar-arb status instead of the historical hardcoded `false`.
// This reuses the same cached SPY board as the fit-accuracy test above, but
// routes it through ConvexDense to reach the CurveSurface override path.
TEST(SpyRealCalendarReporting, DenseSurfaceReportsMeasuredCalendar) {
  const std::string path = find_spy_parquet();
  if (path.empty()) {
    GTEST_SKIP() << "cached SPY OPRA parquet not found; run the databento pull + "
                    "opra_dbn_to_parquet to materialise the fixture.";
  }

  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "SPY";
  spec.snapshot_iso = "2026-06-05T19:55:00Z";
  spec.r = 0.043;
  const auto panel = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  auto in = make_session_inputs(FitPreset::Fast, panel->implied_spot, spec.r,
                                panel->frame.snapshot_ts_ns);
  in.curve.kind = VolCurveKind::ConvexDense;  // route to the CurveSurface override
  const auto sess = VolaSession::from_frame(panel->frame, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const SessionDiagnostics& d = sess->diagnostics();
  std::printf(
      "[SPY dense calendar] calendar_arb_free=%s n_calendar_viol_pre=%zu\n",
      d.calendar_arb_free ? "true" : "false", d.n_calendar_viol_pre);

  // Pre-enforcement this may be >0; the assertion is only that it is COMPUTED:
  // n_calendar_viol_pre is 0 IFF calendar_arb_free is true (they agree). Before
  // this fix, the stamp was hardcoded false/0, which fails this identity.
  EXPECT_EQ(d.calendar_arb_free, d.n_calendar_viol_pre == 0u);
}
