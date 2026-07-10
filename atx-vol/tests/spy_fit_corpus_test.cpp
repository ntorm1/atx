// Real OPRA cold-fit breadth gate: ten SPY slices spanning date, time-of-day,
// and stress regime must each preserve at least 98% clean price-in-NBBO.

#include <chrono>
#include <cstdio>
#include <string>

#include <gtest/gtest.h>

#include "atx/vol/chain.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "support/spy_fit_fixture.hpp"

namespace {

using namespace atx::vol;
using atx::vol::testkit::kSpyFitFixtures;
using atx::vol::testkit::load_spy_fit_fixture;
using atx::vol::testkit::price_in_band;

TEST(SpyFitCorpus, HftColdStartPreserves98PctOnEveryAvailableSlice) {
  std::size_t loaded = 0;
  for (const auto &fixture : kSpyFitFixtures) {
    auto board = load_spy_fit_fixture(fixture);
    if (!board.has_value()) {
      continue;
    }
    ++loaded;
    SCOPED_TRACE(std::string(fixture.id) + " (" + fixture.snapshot_iso + ")");

    auto chain = OptionChain::from_frame(board->panel.frame, board->env());
    ASSERT_TRUE(chain.has_value()) << chain.error().to_string();

    PricerFitter fitter{PricerConfig{.preset = FitPreset::Hft}};
    const auto t0 = std::chrono::steady_clock::now();
    const Status fit = fitter.fit(*chain);
    const auto t1 = std::chrono::steady_clock::now();
    ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    const auto score =
        price_in_band(fitter.surface()->session(), chain->underlying(), board->spot(), board->r);
    std::printf("[SPY fit corpus] %-12s %-24s fit=%7.2fms pxCLN=%6.2f%% "
                "(%zu/%zu) legs=%zu\n",
                fixture.id, fixture.regime, ms, score.px_clean, score.n_clean_in, score.n_clean,
                chain->size());
    EXPECT_GT(score.n_clean, 100u);
    EXPECT_GE(score.px_clean, 98.0);
  }

  if (loaded == 0) {
    GTEST_SKIP() << "SPY fit corpus not found under data/spy_fit_slices";
  }
  EXPECT_EQ(loaded, kSpyFitFixtures.size())
      << "partial SPY corpus: materialize all ten fixtures before benchmarking";
}

} // namespace
