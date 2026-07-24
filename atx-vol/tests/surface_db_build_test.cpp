// GenerateSymbolConfigs suite — proves generate_symbol_configs (Task 4) writes
// one per-symbol SymbolFitConfig into a SurfaceDb manifest from a set of loaded
// OPRA boards: fresh config per symbol, idempotent skip-existing, the
// overwrite escape hatch, the pinned dense index recipe, and fail-closed
// disabling of a symbol whose board cannot be selected on.
//
// BuildSurfaceDb suite (Task 5) — proves the one-call build driver
// (build_surface_db): create-or-open, hive load, config generation, streaming
// populate, and the resume/incremental/no-op semantics, asserting the REOPENED
// db reality (partitions + map_surface), not just the report counters. Plus a
// write_build_report_csv round-trip.
//
// Boards are built from the Task 2 synthetic hive fixture through the real
// Task 3 loader (load_opra_hive) + corpus_board_from_opra, so the selection
// path runs against genuine loader output.

#include "support/synthetic_opra_hive.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// ── BuildSurfaceDb suite (Task 5) ───────────────────────────────────────────

// A fresh temp workspace holding a synthetic hive and a db root under it.
struct BuildFixture {
  fs::path root; // fresh temp dir (removed on construction)
  fs::path hive; // root/hive  — the OPRA hive v2 tree
  fs::path db;   // root/db    — the SurfaceDb root
};

// Materialize `fx` as a hive-v2 tree under a fresh workspace named `name`.
[[nodiscard]] BuildFixture make_build_fixture(std::string_view name,
                                              const tsupport::SyntheticHiveSpec &fx) {
  BuildFixture f;
  f.root = fresh_dir(name);
  f.hive = f.root / "hive";
  f.db = f.root / "db";
  tsupport::write_synthetic_hive_v2(f.hive, fx);
  return f;
}

// A build spec pointing a db at the fixture's hive over `fx`'s full date span,
// loading exactly `fx`'s symbols (the ingest a production build runs).
[[nodiscard]] SurfaceDbBuildSpec build_spec(const BuildFixture &f,
                                            const tsupport::SyntheticHiveSpec &fx) {
  SurfaceDbBuildSpec spec;
  spec.db_root = f.db.string();
  spec.hive.root_dir = f.hive.string();
  spec.hive.date_lo = fx.dates.front();
  spec.hive.date_hi = fx.dates.back();
  spec.hive.symbols = fx.symbols;
  spec.hive.r = fx.r;
  return spec;
}

// Load the fixture hive's boards through the real loader (load_opra_hive ->
// corpus_board_from_opra) — the exact ingest build_surface_db runs, but against
// an EXISTING fixture rather than a freshly made one.
[[nodiscard]] std::vector<CorpusBoard> load_fixture_hive_boards(const BuildFixture &f,
                                                                const tsupport::SyntheticHiveSpec &fx) {
  OpraHiveSpec spec;
  spec.root_dir = f.hive.string();
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

// One call over a fresh 3x3 hive builds an end-to-end db: 3 dates loaded (the
// 07-03/04/05 calendar gap is missing), 3 symbols configured, 9 cells fit, and
// the REOPENED db carries 3 partitions / 3 symbols with a mappable surface.
TEST(BuildSurfaceDb, EndToEndOnFixtureHive) {
  const tsupport::SyntheticHiveSpec fx; // AAA/BBB/CCC x {07-01,07-02,07-06}
  const BuildFixture f = make_build_fixture("e2e", fx);

  const auto rep = build_surface_db(build_spec(f, fx));
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->n_dates_loaded, std::size_t{3});
  EXPECT_EQ(rep->n_dates_missing, std::size_t{3}); // 07-03, 07-04, 07-05 gap
  EXPECT_EQ(rep->n_load_errors, std::size_t{0});
  EXPECT_EQ(rep->config.n_configured, 3u);
  EXPECT_EQ(rep->config.n_disabled_failed, 0u);
  EXPECT_EQ(rep->coverage.cells_to_fit, 9u);
  EXPECT_EQ(rep->coverage.cells_failed, 0u);
  EXPECT_EQ(rep->coverage.cells_ok, 9u); // == cells_to_fit, n_failed == 0
  EXPECT_EQ(rep->coverage.dates_written, 3u);

  // Reopened-db reality (not just the returned counters).
  auto db = SurfaceDb::open(f.db.string());
  ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
  EXPECT_EQ(db->partitions().size(), std::size_t{3});
  EXPECT_EQ(db->symbols().size(), std::size_t{3});
  const auto surf = db->map_surface("2026-07-01", "AAA");
  EXPECT_TRUE(surf.has_value()) << (surf ? "" : surf.error().to_string());
}

// An immediate second build over the same db + hive fits nothing: every cell is
// already present (cells_to_fit == 0, dates_written == 0) and every symbol is
// already configured (skipped, not reconfigured).
TEST(BuildSurfaceDb, RerunFitsZero) {
  const tsupport::SyntheticHiveSpec fx;
  const BuildFixture f = make_build_fixture("rerun", fx);
  const SurfaceDbBuildSpec spec = build_spec(f, fx);

  ASSERT_TRUE(build_surface_db(spec).has_value());
  const auto rep2 = build_surface_db(spec); // opens the existing db
  ASSERT_TRUE(rep2.has_value()) << (rep2 ? "" : rep2.error().to_string());
  EXPECT_EQ(rep2->coverage.cells_to_fit, 0u);
  EXPECT_EQ(rep2->coverage.dates_written, 0u);
  EXPECT_EQ(rep2->coverage.dates_skipped_complete, 3u);
  EXPECT_EQ(rep2->config.n_configured, 0u);
  EXPECT_EQ(rep2->config.n_skipped_existing, 3u);

  auto db = SurfaceDb::open(f.db.string());
  ASSERT_TRUE(db.has_value());
  EXPECT_EQ(db->partitions().size(), std::size_t{3}); // no rewrite
}

// A DISABLED symbol must not keep its dates in the rewrite set forever. Its cell
// can never be added (the population loop skips it, so it never lands in the
// written partition), so counting it as "to add" makes every later run see the
// same permanent gap and rewrite — and therefore RE-FIT — that whole date on
// every run. With the disabled cell excluded from the tally the build reaches a
// fixed point: the second run over an unchanged hive fits ZERO.
TEST(BuildSurfaceDb, RerunFitsZeroWithDisabledSymbol) {
  const tsupport::SyntheticHiveSpec fx; // AAA/BBB/CCC x 3 dates
  const BuildFixture f = make_build_fixture("rerun_disabled", fx);
  const SurfaceDbBuildSpec spec = build_spec(f, fx);

  // Pre-seed the manifest with a DISABLED CCC through the real fail-closed path
  // (SelectionFailureStoredDisabled's technique: gut CCC to a single quote so
  // selection cannot run). generate_symbol_configs is skip-existing, so both
  // builds below leave that disabled config exactly as it is.
  {
    auto db = SurfaceDb::create(f.db.string());
    ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
    std::vector<CorpusBoard> gutted = load_fixture_hive_boards(f, fx);
    ASSERT_EQ(gutted.size(), std::size_t{9});
    for (CorpusBoard &b : gutted) {
      if (b.symbol == "CCC") {
        ASSERT_FALSE(b.frame.rows.empty());
        b.frame.rows.resize(1);
      }
    }
    const auto cfg = generate_symbol_configs(*db, gutted, AutoConfigSpec{});
    ASSERT_TRUE(cfg.has_value()) << (cfg ? "" : cfg.error().to_string());
    ASSERT_EQ(cfg->n_disabled_failed, 1u);
    const auto ccc = db->symbol_config("CCC");
    ASSERT_TRUE(ccc.has_value());
    ASSERT_FALSE(ccc->enabled);
  }

  const auto rep1 = build_surface_db(spec);
  ASSERT_TRUE(rep1.has_value()) << (rep1 ? "" : rep1.error().to_string());
  ASSERT_EQ(rep1->coverage.dates_written, 3u); // AAA/BBB written on all 3 dates

  // Second run over the UNCHANGED hive. CCC is still disabled and still absent
  // from every partition — a gap that can never close — so there is nothing to
  // do: no date is rewritten and no cell is (re)fit.
  const auto rep2 = build_surface_db(spec);
  ASSERT_TRUE(rep2.has_value()) << (rep2 ? "" : rep2.error().to_string());
  EXPECT_EQ(rep2->coverage.cells_to_fit, 0u);
  EXPECT_EQ(rep2->coverage.dates_written, 0u);
  EXPECT_EQ(rep2->coverage.dates_skipped_complete, 3u);
  EXPECT_EQ(rep2->coverage.cells_refit, 0u);
}

// Growing the hive by one date rebuilds ONLY that date: the three prior dates
// are complete-skips, one new partition is written.
TEST(BuildSurfaceDb, IncrementalNewDateOnly) {
  tsupport::SyntheticHiveSpec fx; // 3 dates
  const BuildFixture f = make_build_fixture("incr", fx);
  ASSERT_TRUE(build_surface_db(build_spec(f, fx)).has_value());

  // Add a 4th date and re-materialize the hive in place; rebuild the same db.
  fx.dates.push_back("2026-07-07");
  tsupport::write_synthetic_hive_v2(f.hive, fx);
  const auto rep = build_surface_db(build_spec(f, fx)); // date_hi now 07-07
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->n_dates_loaded, std::size_t{4});
  EXPECT_EQ(rep->coverage.dates_written, 1u);          // only the new date
  EXPECT_EQ(rep->coverage.cells_to_fit, 3u);           // its 3 symbols
  EXPECT_EQ(rep->coverage.dates_skipped_complete, 3u); // the prior dates

  auto db = SurfaceDb::open(f.db.string());
  ASSERT_TRUE(db.has_value());
  EXPECT_EQ(db->partitions().size(), std::size_t{4});
  EXPECT_TRUE(db->map_surface("2026-07-07", "BBB").has_value());
}

// An un-pulled window (empty hive) is a graceful no-op: Ok, all-zero coverage,
// no boards, and the freshly created db has no partitions.
TEST(BuildSurfaceDb, UnpulledWindowGracefulNoop) {
  const fs::path root = fresh_dir("unpulled");
  const fs::path hive = root / "hive";
  fs::create_directories(hive); // empty: no date=<d>/data.parquet under it
  const fs::path db_root = root / "db";

  SurfaceDbBuildSpec spec;
  spec.db_root = db_root.string();
  spec.hive.root_dir = hive.string();
  spec.hive.date_lo = "2026-07-01";
  spec.hive.date_hi = "2026-07-03";
  spec.hive.symbols = {"AAA", "BBB", "CCC"};
  spec.hive.r = 0.03;

  const auto rep = build_surface_db(spec);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->n_dates_loaded, std::size_t{0});
  EXPECT_EQ(rep->n_dates_missing, std::size_t{3});
  EXPECT_EQ(rep->coverage.cells_loaded, 0u);
  EXPECT_EQ(rep->coverage.cells_to_fit, 0u);
  EXPECT_EQ(rep->coverage.cells_ok, 0u);
  EXPECT_EQ(rep->coverage.dates_written, 0u);

  // The db was still created; it simply holds no partitions.
  auto reopened = SurfaceDb::open(db_root.string());
  ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());
  EXPECT_TRUE(reopened->partitions().empty());
}

// write_build_report_csv emits the pinned "key,value" scalar section header on
// line 1, then per-symbol coverage rows; the file exists and carries both.
TEST(BuildSurfaceDb, ReportCsvRoundTrips) {
  const tsupport::SyntheticHiveSpec fx;
  const BuildFixture f = make_build_fixture("csv", fx);
  const auto rep = build_surface_db(build_spec(f, fx));
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());

  const fs::path csv = f.root / "report.csv";
  const Status w = write_build_report_csv(*rep, csv.string());
  ASSERT_TRUE(w.has_value()) << (w ? "" : w.error().to_string());
  ASSERT_TRUE(fs::exists(csv));

  std::ifstream is(csv.string(), std::ios::binary);
  ASSERT_TRUE(is.good());
  std::string first_line;
  std::getline(is, first_line);
  if (!first_line.empty() && first_line.back() == '\r') {
    first_line.pop_back();
  }
  EXPECT_EQ(first_line, "key,value"); // pinned header

  const std::string body((std::istreambuf_iterator<char>(is)),
                         std::istreambuf_iterator<char>());
  EXPECT_NE(body.find("coverage.cells_ok,9"), std::string::npos);
  EXPECT_NE(body.find("symbol,n_attempted,n_ok,n_failed,n_disabled"), std::string::npos);
  EXPECT_NE(body.find("AAA,3,3,0,0"), std::string::npos);
}

} // namespace
} // namespace atx::vol
