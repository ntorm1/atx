// Real OPRA unified-policy breadth gate. Payloads are optional external fixtures;
// a fixture-enabled run requires the complete fourteen-board matrix.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/chain.hpp"
#include "atx/vol/corpus.hpp"
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

TEST(OpraBreadthCorpus, CompleteCachedSetProducesQualifiedScoreboard) {
  struct LoadedFixture {
    const BreadthFitFixture *fixture{nullptr};
    OpraBoard board;
  };
  std::vector<LoadedFixture> loaded;
  loaded.reserve(kBreadthFitFixtures.size());
  for (const BreadthFitFixture &fixture : kBreadthFitFixtures) {
    auto board = load_breadth_fit_fixture(fixture);
    if (board.has_value()) {
      loaded.push_back(LoadedFixture{&fixture, std::move(*board)});
    }
  }
  if (loaded.empty()) {
    GTEST_SKIP() << "OPRA breadth corpus not found under data/vol_breadth_slices";
  }
  ASSERT_EQ(loaded.size(), kBreadthFitFixtures.size())
      << "partial breadth corpus: materialize all fourteen fixtures";

  std::vector<CorpusBoard> boards;
  boards.reserve(loaded.size());
  for (const LoadedFixture &item : loaded) {
    std::string date = item.fixture->snapshot_iso;
    std::replace(date.begin(), date.end(), ':', '-');
    CorpusBoard board;
    board.date = std::move(date);
    board.symbol = item.fixture->symbol;
    board.frame = item.board.panel.frame;
    board.env = item.board.env();
    board.fit_context = breadth_fit_context(*item.fixture);
    board.source_schema_version = item.board.panel.source_schema_version;
    board.source_fingerprint = item.board.panel.source_fingerprint;
    // Cached legacy fixtures predate the instrument-id column. The scoreboard
    // remains diagnostic; strict real-pilot admission requires new v2 pulls.
    board.source_provenance_complete = item.board.panel.provenance_complete;
    boards.push_back(std::move(board));
  }

  QualifiedCorpusConfig config;
  config.admission.enabled = true;
  for (CorpusAdmissionRule &rule : config.admission.by_profile) {
    rule.require_calendar_arb_free = false;
  }
  config.build.write_opts.created_ts_ns = 1;
  const std::filesystem::path out =
      std::filesystem::temp_directory_path() / "atx-opra-qualified-breadth";
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
  auto corpus = build_qualified_corpus(boards, out.string(), config);
  ASSERT_TRUE(corpus.has_value()) << corpus.error().to_string();
  EXPECT_EQ(corpus->quality.n_planned, kBreadthFitFixtures.size());
  EXPECT_EQ(corpus->quality.n_admitted, kBreadthFitFixtures.size());
  EXPECT_EQ(corpus->quality.entries.size(), kBreadthFitFixtures.size());

  const auto vxx =
      std::find_if(corpus->quality.entries.begin(), corpus->quality.entries.end(),
                   [](const QualifiedCorpusEntry &entry) { return entry.symbol == "VXX"; });
  ASSERT_NE(vxx, corpus->quality.entries.end());
  EXPECT_EQ(vxx->quality.profile, ProfileKind::VolProduct)
      << "VXX remains a distinct diagnostic profile";
}

} // namespace
