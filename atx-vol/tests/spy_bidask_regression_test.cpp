// SPY bid-ask accuracy regression — LOCK IN THE DEPTH.
//
// The carry/de-Americanization fix + arb-free convex dense fit reach 99.5%
// price-in-band on the locally-convex (clean) subset of the SPY OPRA board. That
// number was previously only demonstrable in bench code (fit_convex_slice called
// by hand). These tests assert it is MAINTAINED through the production facade:
// the convex surface fit via VolaSession / PricerFitter and SERVED by
// `fair_value` (the price the library actually produces). They also verify the
// CurveSelector auto-picks the dense curve for SPY.
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

// The regression floor. The convex-QP(40n) fit measures ~99.5% pxCLN; the served
// (cached-pricer) round-trip may drift a few hundredths. 99.0 is a firm floor
// that still catches any real regression (the pre-fix number was 65.8%, and the
// eSSVI backbone is ~10% on this metric).
constexpr double kPxCleanFloor = 99.0;

}  // namespace

TEST(SpyBidAskRegression, ConvexDenseServedViaSessionMaintains99) {
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

TEST(SpyBidAskRegression, PricerFitterExplicitConvexMaintains99) {
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
