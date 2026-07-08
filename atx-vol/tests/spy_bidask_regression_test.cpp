// SPY bid-ask accuracy regression — served-surface price-in-band gate.
//
// The carry/de-Americanization fix + arb-free convex dense fit reach ~99.5%
// price-in-band on the locally-convex (clean) subset of the SPY OPRA board. These
// tests assert that depth is served through the production facade (VolaSession /
// PricerFitter -> fair_value, the price the library actually produces), and that
// the CurveSelector auto-picks the dense curve for SPY.
//
// NOTE: the served dense surface now ENFORCES calendar no-arbitrage by construction
// (the sequential ascending-T floor in fit_curve_surface). SPY's front slices carry
// genuine calendar structure, so enforcement trades ~4.8pp of price-in-band
// (~99.5% -> ~94.65%) for a calendar-arb-free surface — a deliberate product choice.
// The gate (kPxCleanFloor) sits below the enforced number; see its comment.
//
// The test GTEST_SKIPs cleanly when the SPY parquet fixture is not present.

#include <cstdio>

#include <gtest/gtest.h>

#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/vol_curve.hpp"
#include "support/opra_fixture.hpp"

using namespace atx::vol;
using atx::vol::testkit::load_opra_board;
using atx::vol::testkit::price_in_band;

namespace {

// The served-surface price-in-band floor. Calendar no-arbitrage is ENFORCED by
// construction on the dense surface (the sequential ascending-T calendar floor in
// fit_curve_surface feeds each fitted slice's variance as the next slice's lower
// bound). On the SPY board the front slices carry GENUINE calendar structure, so
// removing those crossings pulls some marks off-mid: served pxCLN is ~94.65% WITH
// enforcement vs ~99.5% without. That ~4.8pp is the explicit, deliberate cost of a
// calendar-arb-free served surface — an MM product decision (never mark outside the
// tradeable band to chase tightness at the expense of a clean surface), NOT a
// regression. 94.0 is a firm floor below the enforced number that still catches any
// real fit regression (pre-fix was 65.8%; the eSSVI backbone is ~10% on this metric).
constexpr double kPxCleanFloor = 94.0;

}  // namespace

TEST(SpyBidAskRegression, ConvexDenseServedViaSessionInBand) {
  auto board = load_opra_board("spy", "SPY");
  if (!board.has_value()) {
    GTEST_SKIP() << "SPY OPRA parquet fixture not found";
  }

  // Fit the arb-free convex dense curve through the session (the path the fixed
  // carry + de-Am feed). node_cap 40 == the headline bench config.
  SessionInputs in = make_session_inputs(FitPreset::Fast, board->spot(), board->r,
                                         board->now_ns());
  in.cash_divs = board->panel.frame.divs;
  in.curve.kind = VolCurveKind::ConvexDense;
  in.curve.convex.node_cap = 40;

  auto sess = VolaSession::build(board->underlying(), in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const auto sc = price_in_band(*sess, board->underlying(), board->spot(), board->r);
  std::printf(
      "[SPY convex-dense via session] pxCLN(cold)=%.2f%% (%zu/%zu)  "
      "pxALL=%.2f%%  served(cached)=%.2f%%\n",
      sc.px_clean, sc.n_clean_in, sc.n_clean, sc.px_all, sc.px_clean_served);

  ASSERT_GT(sc.n_clean, 100u) << "too few clean quotes to be a meaningful gate";
  EXPECT_GE(sc.px_clean, kPxCleanFloor);
}

TEST(SpyBidAskRegression, PricerFitterExplicitConvexInBand) {
  auto board = load_opra_board("spy", "SPY");
  if (!board.has_value()) {
    GTEST_SKIP() << "SPY OPRA parquet fixture not found";
  }

  // Self-contained facade: MarketEnv in, surface out. Pin the convex curve.
  auto chain = OptionChain::from_frame(board->panel.frame, board->env());
  ASSERT_TRUE(chain.has_value()) << chain.error().to_string();

  PricerConfig cfg;
  cfg.preset = FitPreset::Fast;
  CurveConfig cc;
  cc.kind = VolCurveKind::ConvexDense;
  cc.convex.node_cap = 40;
  cfg.curve = cc;

  PricerFitter fitter{cfg};
  ASSERT_TRUE(fitter.fit(*chain).has_value());
  ASSERT_TRUE(fitter.fitted());

  const auto sc = price_in_band(fitter.surface()->session(), chain->underlying(),
                                board->spot(), board->r);
  std::printf("[SPY PricerFitter(convex)] pxCLN(cold)=%.2f%% (%zu/%zu)\n",
              sc.px_clean, sc.n_clean_in, sc.n_clean);
  EXPECT_GE(sc.px_clean, kPxCleanFloor);
}

TEST(SpyBidAskRegression, AutoSelectPicksDenseForSpy) {
  auto board = load_opra_board("spy", "SPY");
  if (!board.has_value()) {
    GTEST_SKIP() << "SPY OPRA parquet fixture not found";
  }

  // No curve config => the CurveSelector searches. On the penny-dense SPY board it
  // should pick the convex dense curve (it wins out-of-sample).
  auto chain = OptionChain::from_frame(board->panel.frame, board->env());
  ASSERT_TRUE(chain.has_value()) << chain.error().to_string();

  PricerConfig cfg;
  cfg.preset = FitPreset::Fast;  // cfg.curve left unset => auto-select
  PricerFitter fitter{cfg};
  ASSERT_TRUE(fitter.fit(*chain).has_value());

  ASSERT_TRUE(fitter.selection().has_value());
  const auto& sel = *fitter.selection();
  std::printf("[SPY auto-select] chose %s\n", to_string(sel.chosen.kind));
  for (const auto& s : sel.scores) {
    std::printf("    candidate %-13s OOS in-band=%.2f%%  vw=%.2f%%  (%zu held-out)\n",
                to_string(s.kind), 100.0 * s.oos_in_band, 100.0 * s.oos_vw,
                s.n_holdout);
  }
  EXPECT_EQ(sel.chosen.kind, VolCurveKind::ConvexDense);

  // And the served surface holds the headline.
  const auto sc = price_in_band(fitter.surface()->session(), chain->underlying(),
                                board->spot(), board->r);
  std::printf("[SPY auto-select] pxCLN(cold)=%.2f%%\n", sc.px_clean);
  EXPECT_GE(sc.px_clean, kPxCleanFloor);
}
