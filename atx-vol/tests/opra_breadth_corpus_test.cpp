// Real OPRA unified-policy breadth gate. Payloads are optional external fixtures;
// a fixture-enabled run requires the complete fourteen-board matrix.

#include <chrono>
#include <cstdio>
#include <string>

#include <gtest/gtest.h>

#include "atx/vol/chain.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "support/breadth_fit_fixture.hpp"

namespace {

using namespace atx::vol;
using namespace atx::vol::testkit;

TEST(OpraBreadthCorpus, UnifiedPolicyFitsEveryAvailableBoard) {
  std::size_t loaded = 0;
  for (const BreadthFitFixture &fixture : kBreadthFitFixtures) {
    auto board = load_breadth_fit_fixture(fixture);
    if (!board.has_value()) {
      continue;
    }
    ++loaded;
    SCOPED_TRACE(std::string(fixture.id) + " (" + fixture.regime + ")");
    auto chain = OptionChain::from_frame(board->panel.frame, board->env());
    ASSERT_TRUE(chain.has_value()) << chain.error().to_string();

    PricerConfig config;
    config.context = breadth_fit_context(fixture);
    PricerFitter fitter{config};
    const auto t0 = std::chrono::steady_clock::now();
    const Status status = fitter.fit(*chain);
    const auto t1 = std::chrono::steady_clock::now();
    ASSERT_TRUE(status.has_value()) << status.error().to_string();
    ASSERT_TRUE(fitter.decision().has_value());
    const double fit_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const auto profile = profile_lookup(fitter.decision()->profile.kind);
    ASSERT_TRUE(profile.has_value());
    const PxBandScore score = price_in_band(fitter.surface()->session(), chain->underlying(),
                                            board->spot(), board->r, (*profile)->calib);
    std::printf("[breadth corpus] %-11s %-28s curve=%-15s fit=%8.2fms "
                "pxCLN=%6.2f%% (%zu/%zu) legs=%zu\n",
                fixture.id, fixture.regime, to_string(fitter.decision()->curve.kind), fit_ms,
                score.px_clean, score.n_clean_in, score.n_clean, chain->size());
    EXPECT_EQ(fitter.decision()->curve.kind, fixture.expected_curve);
    EXPECT_GT(score.n_clean, 20u);
    EXPECT_GE(score.px_clean, fixture.min_clean_pct);
  }
  if (loaded == 0) {
    GTEST_SKIP() << "OPRA breadth corpus not found under data/vol_breadth_slices";
  }
  EXPECT_EQ(loaded, kBreadthFitFixtures.size())
      << "partial breadth corpus: materialize all fourteen fixtures";
}

} // namespace
