// GenerateSymbolConfigs suite — proves generate_symbol_configs (Task 4) writes
// one per-symbol SymbolFitConfig into a SurfaceDb manifest from a set of loaded
// OPRA boards: fresh config per symbol, idempotent skip-existing, the
// overwrite escape hatch, the pinned dense index recipe, and fail-closed
// disabling of a symbol whose board cannot be selected on.
//
// Boards are built from the Task 2 synthetic hive fixture through the real
// Task 3 loader (load_opra_hive) + corpus_board_from_opra, so the selection
// path runs against genuine loader output.

#include "support/synthetic_opra_hive.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/corpus.hpp"           // CorpusBoard
#include "atx/vol/opra_batch.hpp"       // corpus_board_from_opra
#include "atx/vol/opra_hive.hpp"        // OpraHiveSpec, load_opra_hive
#include "atx/vol/session.hpp"          // FitPreset
#include "atx/vol/surface_db.hpp"       // SurfaceDb, SymbolFitConfig
#include "atx/vol/surface_db_build.hpp" // AutoConfigSpec, AutoConfigReport, generate_symbol_configs
#include "atx/vol/types.hpp"
#include "atx/vol/vol_curve.hpp" // VolCurveKind

namespace atx::vol {
namespace {

namespace fs = std::filesystem;
namespace tsupport = atx::vol::testsupport;

[[nodiscard]] fs::path fresh_dir(std::string_view name) {
  fs::path p = fs::temp_directory_path() / ("atx_surface_db_build_" + std::string(name));
  fs::remove_all(p);
  return p;
}

// Write the default 3-symbol x 3-date synthetic hive under `name`/hive and load
// it into boards through the real loader path (load_opra_hive ->
// corpus_board_from_opra), the exact ingest Task 5's build driver uses.
[[nodiscard]] std::vector<CorpusBoard> load_fixture_boards(std::string_view name) {
  const fs::path root = fresh_dir(name);
  const fs::path hive = root / "hive";
  const tsupport::SyntheticHiveSpec fx; // AAA/BBB/CCC x 3 dates, spot 100
  tsupport::write_synthetic_hive_v2(hive, fx);

  OpraHiveSpec spec;
  spec.root_dir = hive.string();
  spec.date_lo = fx.dates.front();
  spec.date_hi = fx.dates.back();
  spec.symbols = fx.symbols;
  spec.r = fx.r;

  std::vector<CorpusBoard> boards;
  const auto res = load_opra_hive(spec);
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  if (res.has_value()) {
    for (const OpraBatchEntry &e : res->entries) {
      if (e.panel.has_value()) {
        boards.push_back(corpus_board_from_opra(e.date, e.symbol, *e.panel));
      }
    }
  }
  return boards;
}

[[nodiscard]] SurfaceDb make_db(std::string_view name) {
  auto db = SurfaceDb::create((fresh_dir(name) / "db").string());
  EXPECT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
  return std::move(*db);
}

// A fresh db gets one enabled config per distinct symbol, carrying the base
// preset (the fit-policy decision pins the curve family, not the tier).
TEST(GenerateSymbolConfigs, StoresConfigPerSymbol) {
  const std::vector<CorpusBoard> boards = load_fixture_boards("stores");
  ASSERT_EQ(boards.size(), std::size_t{9}); // 3 symbols x 3 present dates
  SurfaceDb db = make_db("stores");

  const auto rep = generate_symbol_configs(db, boards, AutoConfigSpec{});
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->n_symbols, 3u);
  EXPECT_EQ(rep->n_configured, 3u);
  EXPECT_EQ(rep->n_skipped_existing, 0u);
  EXPECT_EQ(rep->n_disabled_failed, 0u);
  EXPECT_TRUE(rep->failed_symbols.empty());

  for (const char *sym : {"AAA", "BBB", "CCC"}) {
    const auto cfg = db.symbol_config(sym);
    ASSERT_TRUE(cfg.has_value()) << sym;
    EXPECT_TRUE(cfg->enabled) << sym;
    EXPECT_EQ(cfg->preset, FitPreset::Populate) << sym;
  }
}

// Re-running over an already-configured db leaves every symbol untouched: all
// skipped, nothing configured, and the manifest generation does not advance
// (no upsert was issued).
TEST(GenerateSymbolConfigs, IdempotentSkipsExisting) {
  const std::vector<CorpusBoard> boards = load_fixture_boards("idem");
  SurfaceDb db = make_db("idem");

  const auto first = generate_symbol_configs(db, boards, AutoConfigSpec{});
  ASSERT_TRUE(first.has_value());
  ASSERT_EQ(first->n_configured, 3u);
  const std::uint64_t gen_after_first = db.generation();

  const auto second = generate_symbol_configs(db, boards, AutoConfigSpec{});
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->n_symbols, 3u);
  EXPECT_EQ(second->n_configured, 0u);
  EXPECT_EQ(second->n_skipped_existing, 3u);
  EXPECT_EQ(second->n_disabled_failed, 0u);
  EXPECT_EQ(db.generation(), gen_after_first); // no upsert -> no generation bump
}

// overwrite_existing=true replaces a hand-edited config with the freshly
// generated one (the edited band_k is restored to the auto value).
TEST(GenerateSymbolConfigs, OverwriteReplacesExisting) {
  const std::vector<CorpusBoard> boards = load_fixture_boards("overwrite");
  SurfaceDb db = make_db("overwrite");

  ASSERT_TRUE(generate_symbol_configs(db, boards, AutoConfigSpec{}).has_value());
  const auto base = db.symbol_config("AAA");
  ASSERT_TRUE(base.has_value());
  const double auto_band_k = base->band_k;

  // Hand-edit AAA (operator override) to a distinct band_k.
  SymbolFitConfig edited = *base;
  edited.band_k = auto_band_k + 4.0;
  ASSERT_TRUE(db.upsert_symbol("AAA", edited).has_value());
  ASSERT_DOUBLE_EQ(db.symbol_config("AAA")->band_k, auto_band_k + 4.0);

  AutoConfigSpec spec;
  spec.overwrite_existing = true;
  const auto rep = generate_symbol_configs(db, boards, spec);
  ASSERT_TRUE(rep.has_value());
  EXPECT_EQ(rep->n_configured, 3u);
  EXPECT_EQ(rep->n_skipped_existing, 0u);
  EXPECT_DOUBLE_EQ(db.symbol_config("AAA")->band_k, auto_band_k); // restored
}

// The index symbol is pinned to the dense index recipe (ConvexDense), which
// differs from the parsimonious family a non-index board is pinned to.
TEST(GenerateSymbolConfigs, IndexSymbolPinnedDense) {
  const std::vector<CorpusBoard> boards = load_fixture_boards("index");
  SurfaceDb db = make_db("index");

  AutoConfigSpec spec;
  spec.index_symbol = "AAA";
  const auto rep = generate_symbol_configs(db, boards, spec);
  ASSERT_TRUE(rep.has_value());
  EXPECT_EQ(rep->n_configured, 3u);

  const auto aaa = db.symbol_config("AAA");
  ASSERT_TRUE(aaa.has_value());
  EXPECT_TRUE(aaa->pin_curve);
  EXPECT_EQ(aaa->curve.kind, VolCurveKind::ConvexDense); // dense index recipe

  const auto bbb = db.symbol_config("BBB");
  ASSERT_TRUE(bbb.has_value());
  // A non-index board is pinned to the fit-policy family, not the dense recipe.
  EXPECT_NE(bbb->curve.kind, VolCurveKind::ConvexDense);
}

// A board gutted to a single quote cannot support curve selection: it is stored
// disabled (fail closed) and recorded, while the top-level call still succeeds.
TEST(GenerateSymbolConfigs, SelectionFailureStoredDisabled) {
  std::vector<CorpusBoard> boards = load_fixture_boards("failure");
  ASSERT_FALSE(boards.empty());
  // Gut every CCC board to one quote so whichever config board is picked is
  // non-selectable (a single strike cannot pin/select a curve).
  for (CorpusBoard &b : boards) {
    if (b.symbol == "CCC") {
      ASSERT_FALSE(b.frame.rows.empty());
      b.frame.rows.resize(1);
    }
  }
  SurfaceDb db = make_db("failure");

  const auto rep = generate_symbol_configs(db, boards, AutoConfigSpec{});
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string()); // top-level Ok
  EXPECT_EQ(rep->n_symbols, 3u);
  EXPECT_EQ(rep->n_configured, 2u);
  EXPECT_EQ(rep->n_disabled_failed, 1u);
  ASSERT_EQ(rep->failed_symbols.size(), std::size_t{1});
  EXPECT_EQ(rep->failed_symbols.front(), "CCC");

  const auto ccc = db.symbol_config("CCC");
  ASSERT_TRUE(ccc.has_value());
  EXPECT_FALSE(ccc->enabled); // stored disabled, never silently served
}

// deep_selection runs the full held-out select_curve search and still produces
// enabled, pinned configs for genuinely fittable boards (smoke coverage of the
// deep path beyond the 5 mandatory cases).
TEST(GenerateSymbolConfigs, DeepSelectionPinsAndEnables) {
  const std::vector<CorpusBoard> boards = load_fixture_boards("deep");
  SurfaceDb db = make_db("deep");

  AutoConfigSpec spec;
  spec.deep_selection = true;
  const auto rep = generate_symbol_configs(db, boards, spec);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->n_symbols, 3u);
  EXPECT_EQ(rep->n_configured + rep->n_disabled_failed, 3u); // every symbol dispositioned

  const auto aaa = db.symbol_config("AAA");
  ASSERT_TRUE(aaa.has_value());
  if (aaa->enabled) {
    EXPECT_TRUE(aaa->pin_curve);
    EXPECT_EQ(aaa->preset, FitPreset::Populate);
  }
}

} // namespace
} // namespace atx::vol
