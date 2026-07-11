// SurfaceDbPopulate suite — proves populate_surface_db fits genuinely
// fittable synthetic boards (the make_board_spec/fit_board pattern from
// dispersion_test.cpp) into a SurfaceDb, honoring per-symbol manifest
// configs (enabled/pin_curve/...), grouping by date, skip-existing resume,
// non-fatal per-board failure recording, and the stats CSV shape.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/corpus.hpp"
#include "atx/vol/data.hpp"              // iso_to_ns, year_fraction
#include "atx/vol/market_env.hpp"
#include "atx/vol/panel.hpp"             // SynthPanelSpec, make_synthetic_american_panel
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/run_report.hpp"        // MetaKv
#include "atx/vol/s3.hpp"                // S3Params
#include "atx/vol/session.hpp"           // FitPreset
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/surface_db.hpp"
#include "atx/vol/surface_db_populate.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/vol_curve.hpp"

namespace atx::vol {
namespace {

std::filesystem::path test_root(std::string_view name) {
  auto p = std::filesystem::temp_directory_path() / ("atx_surface_db_populate_" + std::string(name));
  std::filesystem::remove_all(p);
  return p;
}

constexpr const char *kDate0 = "2026-03-02";
constexpr const char *kDate1 = "2026-03-03";

// A genuinely fittable board spec (the make_board_spec pattern from
// dispersion_test.cpp:76-111): four expiries with a mild declining term
// structure and a 13-strike ladder, robustly fittable.
[[nodiscard]] SynthPanelSpec make_board_spec(const std::string &symbol, const std::string &date,
                                             double spot, double sigma0) {
  SynthPanelSpec s;
  s.uid = symbol;
  s.snapshot_iso = date;
  s.spot = spot;
  s.r = 0.03;
  s.borrow = 0.0;

  struct Row {
    const char *iso;
    double sig;
    double skew_k;
    double c2;
  };
  const Row rows[] = {
      {"2026-04-17", sigma0, -0.55, 0.6},
      {"2026-05-15", sigma0 - 0.02, -0.52, 0.7},
      {"2026-06-19", sigma0 - 0.04, -0.50, 0.8},
      {"2026-09-18", sigma0 - 0.06, -0.46, 0.9},
  };
  for (const Row &r : rows) {
    SynthExpiry e;
    e.expiry_iso = r.iso;
    e.T = year_fraction(date, r.iso);
    const double s2 = 2.0 * std::sqrt(e.T) * r.skew_k;
    e.truth = S3Params{r.sig, s2, r.c2};
    s.expiries.push_back(e);
  }
  for (const double m :
       {0.80, 0.83, 0.87, 0.91, 0.95, 0.98, 1.0, 1.02, 1.05, 1.09, 1.13, 1.17, 1.20}) {
    s.strikes.push_back(spot * m);
  }
  s.half_spread_frac = 0.05;
  s.min_half_spread = 0.05;
  return s;
}

[[nodiscard]] CorpusBoard make_board(const std::string &date, const std::string &symbol,
                                     double spot, double sigma0) {
  const SynthPanelSpec spec = make_board_spec(symbol, date, spot, sigma0);
  auto panel = make_synthetic_american_panel(spec);
  EXPECT_TRUE(panel.has_value()) << (panel ? "" : panel.error().to_string());
  CorpusBoard b;
  b.date = date;
  b.symbol = symbol;
  b.frame = panel->frame;
  b.env = MarketEnv::flat(spec.spot, spec.r, iso_to_ns(date), spec.cash_divs);
  return b;
}

// 2 symbols ("AAA","BBB") x 2 dates ("2026-03-02","2026-03-03").
[[nodiscard]] std::vector<CorpusBoard> make_boards() {
  std::vector<CorpusBoard> boards;
  boards.push_back(make_board(kDate0, "AAA", 100.0, 0.28));
  boards.push_back(make_board(kDate0, "BBB", 60.0, 0.34));
  boards.push_back(make_board(kDate1, "AAA", 101.0, 0.27));
  boards.push_back(make_board(kDate1, "BBB", 61.0, 0.33));
  return boards;
}

[[nodiscard]] std::string read_file(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST(SurfaceDbPopulate, FitsAndStoresPartitionsPerDate) {
  const auto root = test_root("basic");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  const std::vector<CorpusBoard> boards = make_boards();
  const SurfaceDbPopulateConfig cfg;
  auto result = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

  EXPECT_EQ(result->n_boards, 4u);
  EXPECT_EQ(result->n_ok, 4u);
  EXPECT_EQ(result->n_failed, 0u);
  EXPECT_EQ(result->n_dates_written, 2u);
  EXPECT_EQ(result->n_dates_skipped_existing, 0u);
  EXPECT_EQ(db->partitions().size(), 2u);

  auto s = db->load_surface(kDate0, "AAA");
  EXPECT_TRUE(s.has_value()) << (s ? "" : s.error().to_string());
  auto s2 = db->load_surface(kDate1, "BBB");
  EXPECT_TRUE(s2.has_value()) << (s2 ? "" : s2.error().to_string());

  ASSERT_EQ(result->per_symbol.size(), 2u);
  EXPECT_EQ(result->per_symbol[0].symbol, "AAA");
  EXPECT_EQ(result->per_symbol[0].n_attempted, 2u);
  EXPECT_EQ(result->per_symbol[0].n_ok, 2u);
  EXPECT_EQ(result->per_symbol[0].n_failed, 0u);
  EXPECT_EQ(result->per_symbol[0].n_disabled, 0u);
  EXPECT_EQ(result->per_symbol[1].symbol, "BBB");
  EXPECT_EQ(result->per_symbol[1].n_ok, 2u);

  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPopulate, HonorsDisabledSymbol) {
  const auto root = test_root("disabled");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  auto bbb_cfg = symbol_config_from_preset(FitPreset::Fast);
  bbb_cfg.enabled = false;
  ASSERT_TRUE(db->upsert_symbol("BBB", bbb_cfg).has_value());

  const std::vector<CorpusBoard> boards = make_boards();
  const SurfaceDbPopulateConfig cfg;
  auto result = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

  EXPECT_EQ(result->n_dates_written, 2u);
  for (const char *date : {kDate0, kDate1}) {
    auto archive = db->open_partition(date);
    ASSERT_TRUE(archive.has_value()) << date;
    EXPECT_TRUE(archive->map_symbol("AAA").has_value());
    EXPECT_EQ(archive->map_symbol("BBB").error().code(), ErrorCode::NotFound);
  }

  const auto bbb_it =
      std::find_if(result->per_symbol.begin(), result->per_symbol.end(),
                   [](const PopulateSymbolStats &s) { return s.symbol == "BBB"; });
  ASSERT_NE(bbb_it, result->per_symbol.end());
  EXPECT_EQ(bbb_it->n_attempted, 2u);
  EXPECT_EQ(bbb_it->n_disabled, 2u);
  EXPECT_EQ(bbb_it->n_ok, 0u);
  EXPECT_EQ(bbb_it->n_failed, 0u);

  const auto aaa_it =
      std::find_if(result->per_symbol.begin(), result->per_symbol.end(),
                   [](const PopulateSymbolStats &s) { return s.symbol == "AAA"; });
  ASSERT_NE(aaa_it, result->per_symbol.end());
  EXPECT_EQ(aaa_it->n_ok, 2u);

  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPopulate, SkipExistingResumes) {
  const auto root = test_root("resume");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const std::vector<CorpusBoard> boards = make_boards();
  const SurfaceDbPopulateConfig cfg;

  auto first = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
  EXPECT_EQ(first->n_dates_written, 2u);
  EXPECT_EQ(first->n_dates_skipped_existing, 0u);
  const std::uint64_t gen_after_first = db->generation();

  auto second = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());
  EXPECT_EQ(second->n_dates_skipped_existing, 2u);
  EXPECT_EQ(second->n_dates_written, 0u);
  EXPECT_EQ(db->generation(), gen_after_first);

  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPopulate, FailedFitRecordedNotFatal) {
  const auto root = test_root("failed");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  std::vector<CorpusBoard> boards = make_boards();
  // Corrupt the (kDate0, "BBB") board: empty frame -> fit_board Skipped ->
  // populate counts n_failed (no separate "skipped" bucket in populate stats).
  boards[1].frame = QuoteFrame{};

  const SurfaceDbPopulateConfig cfg;
  auto result = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

  EXPECT_EQ(result->n_boards, 4u);
  EXPECT_EQ(result->n_ok, 3u);
  EXPECT_EQ(result->n_failed, 1u);
  EXPECT_EQ(result->n_dates_written, 2u); // kDate0 still written w/ AAA

  auto archive0 = db->open_partition(kDate0);
  ASSERT_TRUE(archive0.has_value());
  EXPECT_TRUE(archive0->map_symbol("AAA").has_value());
  EXPECT_EQ(archive0->map_symbol("BBB").error().code(), ErrorCode::NotFound);

  const auto bbb_it =
      std::find_if(result->per_symbol.begin(), result->per_symbol.end(),
                   [](const PopulateSymbolStats &s) { return s.symbol == "BBB"; });
  ASSERT_NE(bbb_it, result->per_symbol.end());
  EXPECT_EQ(bbb_it->n_attempted, 2u);
  EXPECT_EQ(bbb_it->n_failed, 1u);
  EXPECT_EQ(bbb_it->n_ok, 1u);

  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPopulate, DateWithZeroSuccessfulFitsWritesNoPartition) {
  const auto root = test_root("empty_date");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  std::vector<CorpusBoard> boards = make_boards();
  // Corrupt BOTH kDate0 boards (indices 0="AAA", 1="BBB") -> zero Ok fits for
  // kDate0 -> no partition written for it (the archive writer rejects an
  // empty item list); kDate1 is untouched and still writes normally.
  boards[0].frame = QuoteFrame{};
  boards[1].frame = QuoteFrame{};

  const SurfaceDbPopulateConfig cfg;
  auto result = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

  EXPECT_EQ(result->n_boards, 4u);
  EXPECT_EQ(result->n_ok, 2u);
  EXPECT_EQ(result->n_failed, 2u);
  EXPECT_EQ(result->n_dates_written, 1u); // only kDate1
  EXPECT_EQ(db->partitions().size(), 1u);
  EXPECT_EQ(db->open_partition(kDate0).error().code(), ErrorCode::NotFound);
  EXPECT_TRUE(db->open_partition(kDate1).has_value());

  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPopulate, StatsCsvShape) {
  const auto root = test_root("stats");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  // Pin AAA (no OOS score -> mean_oos_in_band NaN in its row); BBB uses the
  // default fallback (auto-selected curve -> a real OOS score).
  auto aaa_cfg = symbol_config_from_preset(FitPreset::Fast);
  aaa_cfg.pin_curve = true;
  aaa_cfg.curve = CurveConfig{}; // ConvexDense, node_cap 40
  ASSERT_TRUE(db->upsert_symbol("AAA", aaa_cfg).has_value());

  const std::vector<CorpusBoard> boards = make_boards();
  const SurfaceDbPopulateConfig cfg;
  auto result = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
  ASSERT_EQ(result->n_ok, 4u);

  const auto csv_path = root / "stats.csv";
  const MetaKv meta{{"run", "test"}};
  const Status w = write_populate_stats_csv(*result, meta, csv_path.string());
  ASSERT_TRUE(w.has_value()) << (w ? "" : w.error().to_string());

  const std::string text = read_file(csv_path);
  EXPECT_NE(text.find("# run=test\n"), std::string::npos);
  EXPECT_NE(text.find("symbol,n_attempted,n_ok,n_failed,n_disabled,success_rate,mean_oos_in_band\n"),
            std::string::npos)
      << text;
  // AAA: n_attempted=2, n_ok=2, n_failed=0, n_disabled=0 -> success_rate=1;
  // pinned curve -> no OOS score -> "nan" (the NaN-when-unavailable rule).
  EXPECT_NE(text.find("AAA,2,2,0,0,1,nan\n"), std::string::npos) << text;
  // BBB: same counts (a directly-routed, non-ambiguous board also has no
  // selector OOS score even though its curve isn't pinned -- fit_board's
  // `oos_in_band_available` is tied to the selector having run at all, not
  // to pin_curve specifically; mirrors corpus.cpp's CorpusEntry.oos_in_band).
  EXPECT_NE(text.find("BBB,2,2,0,0,1,nan\n"), std::string::npos) << text;

  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPopulate, PinnedConfigHonored) {
  const auto root = test_root("pinned");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  auto aaa_cfg = symbol_config_from_preset(FitPreset::Fast);
  aaa_cfg.pin_curve = true;
  aaa_cfg.curve.kind = VolCurveKind::ConvexDense;
  aaa_cfg.curve.convex.node_cap = 48;
  ASSERT_TRUE(db->upsert_symbol("AAA", aaa_cfg).has_value());

  const std::vector<CorpusBoard> boards = make_boards();
  const SurfaceDbPopulateConfig cfg;
  auto result = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

  auto s = db->load_surface(kDate0, "AAA");
  ASSERT_TRUE(s.has_value()) << (s ? "" : s.error().to_string());
  EXPECT_EQ(s->kind_at(0), VolCurveKind::ConvexDense);

  std::filesystem::remove_all(root);
}

} // namespace atx::vol
