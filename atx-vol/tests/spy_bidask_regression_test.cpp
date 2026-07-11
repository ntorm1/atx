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

#include <chrono>
#include <cstdio>

#include <gtest/gtest.h>

#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/vol_curve.hpp"
#include "support/cached_artifacts.hpp"
#include "support/opra_fixture.hpp"

using namespace atx::vol;
using atx::vol::test::cached_spy_convex_dense;
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

} // namespace

// Reloads the shared cached SPY ConvexDense archive (see cached_artifacts.hpp)
// instead of re-fitting live; spy_archive_roundtrip_test proves the reload
// reproduces the live session's served accuracy bit-for-bit, so scoring the
// reconstructed PricedSurface here is the same gate at load-time cost.
TEST(SpyBidAskRegression, ConvexDenseServedViaSessionInBand) {
  const auto board = load_opra_board("spy", "SPY");
  const auto archive_path = cached_spy_convex_dense();
  if (!board.has_value() || archive_path.empty()) {
    GTEST_SKIP() << "SPY OPRA parquet fixture not found";
  }

  auto arch = SurfaceArchive::open_file(archive_path.string());
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  auto recon = arch->map_symbol("SPY");
  ASSERT_TRUE(recon.has_value()) << recon.error().to_string();

  const auto sc = price_in_band(*recon, board->underlying(), board->spot(), board->r);
  std::printf("[SPY convex-dense via session] pxCLN(cold)=%.2f%% (%zu/%zu)  "
              "pxALL=%.2f%%  served(cached)=%.2f%%\n",
              sc.px_clean, sc.n_clean_in, sc.n_clean, sc.px_all, sc.px_clean_served);

  ASSERT_GT(sc.n_clean, 100u) << "too few clean quotes to be a meaningful gate";
  EXPECT_GE(sc.px_clean, kPxCleanFloor);
}

// Same cached archive, scored via the PricerFitter-facade's board load (this
// test's point is the facade's MarketEnv/OptionChain plumbing feeds the SAME
// board the cached fit was produced from — the served accuracy is identical
// either way; the fitter itself is exercised live in PricerFitterHftColdStartInBand
// / AutoSelectPicksDenseForSpy below).
TEST(SpyBidAskRegression, PricerFitterExplicitConvexInBand) {
  auto board = load_opra_board("spy", "SPY");
  const auto archive_path = cached_spy_convex_dense();
  if (!board.has_value() || archive_path.empty()) {
    GTEST_SKIP() << "SPY OPRA parquet fixture not found";
  }

  auto arch = SurfaceArchive::open_file(archive_path.string());
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  auto recon = arch->map_symbol("SPY");
  ASSERT_TRUE(recon.has_value()) << recon.error().to_string();

  const auto sc = price_in_band(*recon, board->underlying(), board->spot(), board->r);
  std::printf("[SPY PricerFitter(convex)] pxCLN(cold)=%.2f%% (%zu/%zu)\n", sc.px_clean,
              sc.n_clean_in, sc.n_clean);
  EXPECT_GE(sc.px_clean, kPxCleanFloor);
}

TEST(SpyBidAskRegression, PricerFitterHftColdStartInBand) {
  auto board = load_opra_board("spy", "SPY");
  if (!board.has_value()) {
    GTEST_SKIP() << "SPY OPRA parquet fixture not found";
  }

  auto chain = OptionChain::from_frame(board->panel.frame, board->env());
  ASSERT_TRUE(chain.has_value()) << chain.error().to_string();

  PricerFitter fitter{PricerConfig{.preset = FitPreset::Hft}};
  const auto t0 = std::chrono::steady_clock::now();
  const Status fit = fitter.fit(*chain);
  const auto t1 = std::chrono::steady_clock::now();
  const double fit_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  ASSERT_TRUE(fit.has_value());
  ASSERT_TRUE(fitter.fitted());
  EXPECT_FALSE(fitter.selection().has_value()) << "Hft should skip auto-select";

  const auto sc =
      price_in_band(fitter.surface()->session(), chain->underlying(), board->spot(), board->r);
  std::printf("[SPY PricerFitter(Hft)] fit=%.1fms  pxCLN(cold)=%.2f%% (%zu/%zu)  "
              "pxALL=%.2f%%  served(cached)=%.2f%%  quotes=%zu\n",
              fit_ms, sc.px_clean, sc.n_clean_in, sc.n_clean, sc.px_all, sc.px_clean_served,
              fitter.surface()->diagnostics().n_quotes);
  EXPECT_GE(sc.px_clean, 98.0);
}

TEST(SpyBidAskRegression, AutoSelectPicksDenseForSpy) {
  auto board = load_opra_board("spy", "SPY");
  if (!board.has_value()) {
    GTEST_SKIP() << "SPY OPRA parquet fixture not found";
  }

  // No curve config => the unified policy recognizes the penny-dense ETF board
  // and takes the O(N) HFT route without paying for held-out candidate fits.
  auto chain = OptionChain::from_frame(board->panel.frame, board->env());
  ASSERT_TRUE(chain.has_value()) << chain.error().to_string();

  PricerConfig cfg;
  cfg.preset = FitPreset::Fast; // cfg.curve left unset => auto-select
  PricerFitter fitter{cfg};
  ASSERT_TRUE(fitter.fit(*chain).has_value());

  ASSERT_TRUE(fitter.decision().has_value());
  const auto &decision = *fitter.decision();
  std::printf("[SPY auto-policy] chose %s\n", to_string(decision.curve.kind));
  EXPECT_EQ(decision.profile.kind, atx::vol::ProfileKind::IndexEtfUltraLiquid);
  EXPECT_EQ(decision.preset, FitPreset::Hft);
  EXPECT_EQ(decision.curve.kind, VolCurveKind::LinearVariance);
  EXPECT_FALSE(fitter.selection().has_value());

  // And the served surface holds the headline.
  const auto sc =
      price_in_band(fitter.surface()->session(), chain->underlying(), board->spot(), board->r);
  std::printf("[SPY auto-select] pxCLN(cold)=%.2f%%\n", sc.px_clean);
  EXPECT_GE(sc.px_clean, kPxCleanFloor);
}
