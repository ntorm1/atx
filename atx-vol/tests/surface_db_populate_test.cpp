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

#include "atx/vol/american.hpp"           // AlOpts
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

// Discriminates "the session_overlay lambda populate_surface_db passes into
// fit_board actually reached PricerFitter::fit's SessionInputs" from "the
// per-symbol config was resolved but silently dropped before the fit ran"
// (the finding: PinnedConfigHonored's node_cap=48/kind_at(0) probe passes
// even though ConvexDense would have been auto-selected anyway, and no test
// exercises a field PricerConfig cannot carry at all).
//
// al_override/al is exactly such a field: pricer_config_for_symbol's
// PricerConfig translation (this file, above) has no al_opts member
// whatsoever, so the ONLY way a manifest's al_override can ever reach
// SessionInputs::deam.al_opts is apply_symbol_config running inside the
// session_overlay hook (surface_db.cpp's apply_symbol_config,
// pricer_fitter.cpp:166-168). Both configs below pin the SAME preset
// (FitPreset::Fast) so PricerConfig(A) and PricerConfig(B) are IDENTICAL --
// isolating the comparison to al_override/al alone (a preset mismatch would
// also change PricerConfig::preset, confounding "did the overlay run" with
// "was a different preset selected").
TEST(SurfaceDbPopulate, SymbolConfigOverlayReachesFit) {
  const std::vector<CorpusBoard> single_board = {make_board(kDate0, "AAA", 100.0, 0.28)};
  const double probe_K = 100.0;
  const double probe_T = year_fraction(kDate0, "2026-04-17");

  // A distinctive AlOpts (all 4 fields differ from al_fast_opts()'s
  // {7,16,4,1e-8} -- see american.hpp) that only apply_symbol_config's
  // al_override branch can install.
  AlOpts distinctive_al;
  distinctive_al.n_collocation = 10;
  distinctive_al.n_quadrature = 20;
  distinctive_al.max_newton_iter = 6;
  distinctive_al.tol = 1.0e-9;

  SymbolFitConfig fallback_a;
  fallback_a.preset = FitPreset::Fast;
  fallback_a.al_override = false; // baseline: preset's own al_opts stands

  SymbolFitConfig cfg_b;
  cfg_b.preset = FitPreset::Fast; // SAME preset as fallback_a
  cfg_b.al_override = true;
  cfg_b.al = distinctive_al;

  // db A: AAA absent from the manifest -> resolves to `fallback_a`.
  const auto root_a = test_root("overlay_a");
  auto db_a = SurfaceDb::create(root_a.string());
  ASSERT_TRUE(db_a.has_value());
  SurfaceDbPopulateConfig cfg_a;
  cfg_a.fallback = fallback_a;
  auto result_a = populate_surface_db(*db_a, single_board, cfg_a);
  ASSERT_TRUE(result_a.has_value()) << (result_a ? "" : result_a.error().to_string());
  ASSERT_EQ(result_a->n_ok, 1u);
  auto s_a = db_a->load_surface(kDate0, "AAA");
  ASSERT_TRUE(s_a.has_value()) << (s_a ? "" : s_a.error().to_string());

  // db B: AAA's manifest entry carries the al_override.
  const auto root_b = test_root("overlay_b");
  auto db_b = SurfaceDb::create(root_b.string());
  ASSERT_TRUE(db_b.has_value());
  ASSERT_TRUE(db_b->upsert_symbol("AAA", cfg_b).has_value());
  auto result_b = populate_surface_db(*db_b, single_board, SurfaceDbPopulateConfig{});
  ASSERT_TRUE(result_b.has_value()) << (result_b ? "" : result_b.error().to_string());
  ASSERT_EQ(result_b->n_ok, 1u);
  auto s_b = db_b->load_surface(kDate0, "AAA");
  ASSERT_TRUE(s_b.has_value()) << (s_b ? "" : s_b.error().to_string());

  // ── Discriminating assertions: A (no override) must differ from B ───────
  // Structural: the stored PricedSurface's PricingContext::al_opts (which
  // to_priced_surface() stamps straight from the fit's resolved
  // SessionInputs::deam.al_opts) differs bit-exactly.
  const AlOpts &al_a = s_a->pricing().al_opts;
  const AlOpts &al_b = s_b->pricing().al_opts;
  EXPECT_NE(al_a.n_collocation, al_b.n_collocation);
  EXPECT_NE(al_a.n_quadrature, al_b.n_quadrature);
  EXPECT_NE(al_a.max_newton_iter, al_b.max_newton_iter);
  EXPECT_NE(al_a.tol, al_b.tol);
  // B's stored al_opts equal exactly the manifest's distinctive value -- the
  // VALUE reached the fit, not just "some field changed".
  EXPECT_EQ(al_b.n_collocation, distinctive_al.n_collocation);
  EXPECT_EQ(al_b.n_quadrature, distinctive_al.n_quadrature);
  EXPECT_EQ(al_b.max_newton_iter, distinctive_al.max_newton_iter);
  EXPECT_EQ(al_b.tol, distinctive_al.tol);

  // Behavioral: a different Andersen-Lake discretization re-prices the SAME
  // (K, T, side) on the SAME fitted board to a genuinely different American
  // value -- not just a different label on an otherwise-identical surface.
  auto fv_a = s_a->fair_value(probe_K, probe_T, Side::Call);
  auto fv_b = s_b->fair_value(probe_K, probe_T, Side::Call);
  ASSERT_TRUE(fv_a.has_value()) << (fv_a ? "" : fv_a.error().to_string());
  ASSERT_TRUE(fv_b.has_value()) << (fv_b ? "" : fv_b.error().to_string());
  EXPECT_NE(*fv_a, *fv_b);

  // ── Flake guard: the SAME manifest config into a fresh db reproduces the
  // SAME surface (rules out "A and B just happened to differ this run"). ──
  const auto root_c = test_root("overlay_c");
  auto db_c = SurfaceDb::create(root_c.string());
  ASSERT_TRUE(db_c.has_value());
  ASSERT_TRUE(db_c->upsert_symbol("AAA", cfg_b).has_value());
  auto result_c = populate_surface_db(*db_c, single_board, SurfaceDbPopulateConfig{});
  ASSERT_TRUE(result_c.has_value()) << (result_c ? "" : result_c.error().to_string());
  auto s_c = db_c->load_surface(kDate0, "AAA");
  ASSERT_TRUE(s_c.has_value()) << (s_c ? "" : s_c.error().to_string());

  const AlOpts &al_c = s_c->pricing().al_opts;
  EXPECT_EQ(al_b.n_collocation, al_c.n_collocation);
  EXPECT_EQ(al_b.n_quadrature, al_c.n_quadrature);
  EXPECT_EQ(al_b.max_newton_iter, al_c.max_newton_iter);
  EXPECT_EQ(al_b.tol, al_c.tol);
  auto fv_c = s_c->fair_value(probe_K, probe_T, Side::Call);
  ASSERT_TRUE(fv_c.has_value()) << (fv_c ? "" : fv_c.error().to_string());
  EXPECT_EQ(*fv_b, *fv_c);

  std::filesystem::remove_all(root_a);
  std::filesystem::remove_all(root_b);
  std::filesystem::remove_all(root_c);
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
