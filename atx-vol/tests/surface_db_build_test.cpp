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
// SurfaceDbTotalFitFailure suite — proves `is_total_fit_failure`, the predicate
// the build CLI turns into a NON-zero exit code: a build that attempted work and
// fitted nothing is a failure, while partial coverage and the nothing-to-do
// resume path are both successes.
//
// Boards are built from the Task 2 synthetic hive fixture through the real
// Task 3 loader (load_opra_hive) + corpus_board_from_opra, so the selection
// path runs against genuine loader output.

#include "support/synthetic_opra_hive.hpp"

#include <algorithm>
#include <cstdint>
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
#include "atx/vol/profile.hpp"          // ProfileKind (FitContext::profile_override)
#include "atx/vol/session.hpp"          // FitPreset
#include "atx/vol/surface_db.hpp"       // SurfaceDb, SymbolFitConfig
#include "atx/vol/surface_db_build.hpp" // AutoConfigSpec, AutoConfigReport, generate_symbol_configs
#include "atx/vol/surface_db_populate.hpp" // UniversePopulateSpec, populate_universe_streaming
#include "atx/vol/surface_policy.hpp"      // has_output, SurfacePurpose
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

// The index symbol takes the dense index recipe (ConvexDense), which differs
// from the parsimonious family a non-index board's policy picks.
TEST(GenerateSymbolConfigs, IndexSymbolGetsDenseRecipe) {
  const std::vector<CorpusBoard> boards = load_fixture_boards("index");
  SurfaceDb db = make_db("index");

  AutoConfigSpec spec;
  spec.index_symbol = "AAA";
  const auto rep = generate_symbol_configs(db, boards, spec);
  ASSERT_TRUE(rep.has_value());
  EXPECT_EQ(rep->n_configured, 3u);

  const auto aaa = db.symbol_config("AAA");
  ASSERT_TRUE(aaa.has_value());
  EXPECT_EQ(aaa->curve.kind, VolCurveKind::ConvexDense); // dense index recipe

  const auto bbb = db.symbol_config("BBB");
  ASSERT_TRUE(bbb.has_value());
  // A non-index board records the fit-policy family, not the dense recipe.
  EXPECT_NE(bbb->curve.kind, VolCurveKind::ConvexDense);
}

// ── FIX-B: the curve family is a PREFERRED ROUTE, not a pin, unless asked ─────
//
// A pinned config makes `PricerFitter::auto_routed` false, which switches OFF
// both recovery ladders (construction-failure and admission-rejection) for every
// cell of that symbol. This stage used to pin unconditionally, so the ladders
// were dead for 100% of a production universe and a marginal admission rejection
// became a hard cell loss. The pin is now the operator's explicit choice.

// DEFAULT: no symbol is pinned — neither the policy-routed names nor the index
// leg — while the chosen family is still recorded for the operator to read.
TEST(GenerateSymbolConfigs, DoesNotPinCurveFamilyByDefault) {
  const std::vector<CorpusBoard> boards = load_fixture_boards("nopin");
  SurfaceDb db = make_db("nopin");

  AutoConfigSpec spec;
  spec.index_symbol = "AAA"; // the index leg is the other pin site
  const auto rep = generate_symbol_configs(db, boards, spec);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  ASSERT_EQ(rep->n_configured, 3u);

  for (const char *sym : {"AAA", "BBB", "CCC"}) {
    const auto cfg = db.symbol_config(sym);
    ASSERT_TRUE(cfg.has_value()) << sym;
    EXPECT_TRUE(cfg->enabled) << sym;
    EXPECT_FALSE(cfg->pin_curve) << sym; // the ladder stays alive
  }
  // Unpinned does NOT mean unrecorded: the selected family is still stored.
  EXPECT_EQ(db.symbol_config("AAA")->curve.kind, VolCurveKind::ConvexDense);
  EXPECT_NE(db.symbol_config("BBB")->curve.kind, VolCurveKind::ConvexDense);
}

// The knob restores the old behaviour, at BOTH sites: the policy route and the
// index leg. Asserted on the stored config, not on a log line.
TEST(GenerateSymbolConfigs, PinCurveFamilyKnobPinsBothRoutes) {
  const std::vector<CorpusBoard> boards = load_fixture_boards("dopin");
  SurfaceDb db = make_db("dopin");

  AutoConfigSpec spec;
  spec.index_symbol = "AAA";
  spec.pin_curve_family = true;
  const auto rep = generate_symbol_configs(db, boards, spec);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  ASSERT_EQ(rep->n_configured, 3u);

  for (const char *sym : {"AAA", "BBB", "CCC"}) {
    const auto cfg = db.symbol_config(sym);
    ASSERT_TRUE(cfg.has_value()) << sym;
    EXPECT_TRUE(cfg->pin_curve) << sym;
  }
  EXPECT_EQ(db.symbol_config("AAA")->curve.kind, VolCurveKind::ConvexDense);
}

// A board whose fit policy routes to LinearVariance is a guaranteed 100% cell
// loss ONCE PINNED: the risk pipeline refuses the config outright, so every cell
// fails identically with `invalid correctness policy for requested risk surface`
// and the operator sees only a `cells_failed` number. It is rejected at CONFIG
// time instead — disabled, named in `failed_symbols` — and its cells are never
// attempted. `profile_override` is the same seam the ticker-seed table uses for a
// real index/ETF name that is not the one `--index` slot.
TEST(GenerateSymbolConfigs, PinnedLinearVarianceRejectedAtConfigTime) {
  std::vector<CorpusBoard> boards = load_fixture_boards("linvar_pin");
  ASSERT_FALSE(boards.empty());
  for (CorpusBoard &b : boards) {
    if (b.symbol == "BBB") {
      b.fit_context.profile_override = ProfileKind::IndexEtfUltraLiquid;
    }
  }
  SurfaceDb db = make_db("linvar_pin");

  AutoConfigSpec spec;
  spec.pin_curve_family = true;
  const auto rep = generate_symbol_configs(db, boards, spec);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string()); // top-level Ok
  EXPECT_EQ(rep->n_symbols, 3u);
  EXPECT_EQ(rep->n_configured, 2u);
  EXPECT_EQ(rep->n_disabled_failed, 1u);
  ASSERT_EQ(rep->failed_symbols.size(), std::size_t{1});
  EXPECT_EQ(rep->failed_symbols.front(), "BBB");

  const auto bbb = db.symbol_config("BBB");
  ASSERT_TRUE(bbb.has_value());
  EXPECT_FALSE(bbb->enabled); // fail closed, never silently served

  // ...and the populate never attempts a fit for it: 3 disabled cells, 0 failed,
  // and nothing in the per-cell failure channel.
  UniversePopulateSpec pspec;
  pspec.preset = FitPreset::Populate;
  pspec.fit_workers = 1u;
  const auto cov = populate_universe_streaming(db, boards, pspec);
  ASSERT_TRUE(cov.has_value()) << (cov ? "" : cov.error().to_string());
  EXPECT_EQ(cov->cells_failed, 0u);
  EXPECT_TRUE(cov->failed_cells.empty());
  bool saw_bbb = false;
  for (const PopulateSymbolStats &s : cov->per_symbol) {
    if (s.symbol != "BBB") {
      continue;
    }
    saw_bbb = true;
    EXPECT_EQ(s.n_disabled, 3u);
    EXPECT_EQ(s.n_ok, 0u);
    EXPECT_EQ(s.n_failed, 0u);
  }
  EXPECT_TRUE(saw_bbb);
}

// The guard's SECOND conjunct: the clause it defends lives inside the RISK build,
// which the fitter skips outright for a mark-only request — and that mark path
// pins LinearVariance itself. `symbol_config_from_preset(Hft)` maps to
// MarketMark-only, so a pinned LinearVariance under `--preset hft` is the family's
// intended home and must NOT be disabled; rejecting it would lose cells that fit.
TEST(GenerateSymbolConfigs, PinnedLinearVarianceKeptForAMarkOnlyPreset) {
  std::vector<CorpusBoard> boards = load_fixture_boards("linvar_mark");
  ASSERT_FALSE(boards.empty());
  for (CorpusBoard &b : boards) {
    if (b.symbol == "BBB") {
      b.fit_context.profile_override = ProfileKind::IndexEtfUltraLiquid;
    }
  }
  SurfaceDb db = make_db("linvar_mark");

  AutoConfigSpec spec;
  spec.preset = FitPreset::Hft; // -> SurfaceOutputs::MarketMark, no Risk output
  spec.pin_curve_family = true;
  const auto rep = generate_symbol_configs(db, boards, spec);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->n_configured, 3u);
  EXPECT_EQ(rep->n_disabled_failed, 0u);
  EXPECT_TRUE(rep->failed_symbols.empty());

  const auto bbb = db.symbol_config("BBB");
  ASSERT_TRUE(bbb.has_value());
  EXPECT_TRUE(bbb->enabled); // kept: the mark path fits exactly this family
  EXPECT_TRUE(bbb->pin_curve);
  EXPECT_EQ(bbb->curve.kind, VolCurveKind::LinearVariance);
  EXPECT_FALSE(has_output(bbb->surface_policy.outputs, SurfacePurpose::Risk));
}

// The same board UNPINNED is not a defect: the fitter substitutes ConvexDense for
// a LinearVariance auto-route decision, so disabling the symbol here would throw
// away cells that fit fine. The guard is scoped to the pin that makes it fatal.
TEST(GenerateSymbolConfigs, UnpinnedLinearVarianceIsNotRejected) {
  std::vector<CorpusBoard> boards = load_fixture_boards("linvar_nopin");
  ASSERT_FALSE(boards.empty());
  for (CorpusBoard &b : boards) {
    if (b.symbol == "BBB") {
      b.fit_context.profile_override = ProfileKind::IndexEtfUltraLiquid;
    }
  }
  SurfaceDb db = make_db("linvar_nopin");

  const auto rep = generate_symbol_configs(db, boards, AutoConfigSpec{}); // default: no pin
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->n_configured, 3u);
  EXPECT_EQ(rep->n_disabled_failed, 0u);

  const auto bbb = db.symbol_config("BBB");
  ASSERT_TRUE(bbb.has_value());
  EXPECT_TRUE(bbb->enabled);
  EXPECT_FALSE(bbb->pin_curve);
  EXPECT_EQ(bbb->curve.kind, VolCurveKind::LinearVariance); // recorded, not pinned
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
// enabled configs for genuinely fittable boards (smoke coverage of the deep path
// beyond the 5 mandatory cases). Its winner obeys `pin_curve_family` like the
// policy route does — the ladder is not something the deep search opts out of.
TEST(GenerateSymbolConfigs, DeepSelectionEnablesAndHonorsThePinKnob) {
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
    EXPECT_FALSE(aaa->pin_curve); // default: preferred route, ladder alive
    EXPECT_EQ(aaa->preset, FitPreset::Populate);
  }

  SurfaceDb pinned_db = make_db("deep_pinned");
  AutoConfigSpec pinned = spec;
  pinned.pin_curve_family = true;
  const auto pinned_rep = generate_symbol_configs(pinned_db, boards, pinned);
  ASSERT_TRUE(pinned_rep.has_value()) << (pinned_rep ? "" : pinned_rep.error().to_string());
  const auto pinned_aaa = pinned_db.symbol_config("AAA");
  ASSERT_TRUE(pinned_aaa.has_value());
  if (pinned_aaa->enabled) {
    EXPECT_TRUE(pinned_aaa->pin_curve);
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
  EXPECT_EQ(rep->n_coverage_holes, std::size_t{0}); // uniform hive: no holes either
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

// Non-uniform per-date coverage — the SHAPE OF EVERY REAL HIVE — must report as
// coverage holes, NOT as load errors. A discover-all build lays a rectangular
// date x union grid, so every name absent from a given day is an erroring cell;
// counting those as `n_load_errors` would make a healthy sparse universe read as
// "the hive is corrupt" and let real corruption hide in the noise.
TEST(BuildSurfaceDb, CoverageHolesAreNotLoadErrors) {
  tsupport::SyntheticHiveSpec fx; // AAA/BBB/CCC x {07-01, 07-02, 07-06}
  const BuildFixture f = make_build_fixture("holes", fx);

  // Rewrite 07-02 with only {AAA, CCC}: BBB is a hole on exactly that date.
  const fs::path tmp2 = fresh_dir("holes_d2");
  tsupport::SyntheticHiveSpec d2only;
  d2only.dates = {fx.dates[1]};
  d2only.symbols = {"AAA", "CCC"};
  tsupport::write_synthetic_hive_v2(tmp2, d2only);
  std::error_code fec;
  fs::remove(f.hive / ("date=" + fx.dates[1]) / "data.parquet", fec);
  fs::copy_file(tmp2 / ("date=" + fx.dates[1]) / "data.parquet",
                f.hive / ("date=" + fx.dates[1]) / "data.parquet", fec);
  ASSERT_FALSE(fec) << fec.message();

  SurfaceDbBuildSpec spec = build_spec(f, fx);
  spec.hive.symbols = {}; // discover-all: the production universe build

  const auto rep = build_surface_db(spec);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->n_coverage_holes, std::size_t{1}); // (BBB, 07-02) — sparse, fine
  EXPECT_EQ(rep->n_load_errors, std::size_t{0});    // nothing is actually broken
  EXPECT_EQ(rep->n_dates_loaded, std::size_t{3});
  EXPECT_EQ(rep->coverage.cells_to_fit, 8u); // the 9-cell grid minus the hole
  EXPECT_EQ(rep->coverage.cells_ok, 8u);
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
  // FIX-D fix-1 (I2). The header string is a pinned contract and it MOVED: the
  // carried counters are the only operator-visible evidence that carry-over
  // engaged, now that `is_total_fit_failure` no longer flags the carried-only
  // resume. Both additions APPEND, so the older columns keep their positions.
  EXPECT_NE(body.find("coverage.cells_carried,0"), std::string::npos) << body;
  EXPECT_NE(body.find("symbol,n_attempted,n_ok,n_failed,n_disabled,n_carried"), std::string::npos)
      << body;
  EXPECT_NE(body.find("AAA,3,3,0,0,0"), std::string::npos) << body;
  // Section 3 exists even on a clean build (header, no rows) so a consumer can
  // parse the same shape whether or not anything failed.
  EXPECT_NE(body.find("date,symbol,code,detail"), std::string::npos);
}

// ── FIX-A: the fit stage names its failures, like the config stage always has ──
//
// `config.failed_symbols` has always named the symbols CONFIG SELECTION refused.
// A symbol whose FIT failed named nothing — the operator got `cells_failed 9` and
// no reason, and root-causing it needed a source investigation. These tests pin
// the reason all the way from PricerFitter to the report, the CSV, and the
// display cap.

// End to end through `build_surface_db`: a symbol whose stored config makes the
// risk request unserviceable (pin + LinearVariance — the guard in
// pricer_fitter.cpp's input validation) fails every one of its cells, and each
// failure reaches SurfaceDbBuildReport carrying the FITTER's own text.
//
// Seeded before the build so generate_symbol_configs' skip-existing leaves it
// alone (the RerunFitsZeroWithDisabledSymbol technique).
TEST(BuildSurfaceDb, FailedCellsCarryTheFitReasonIntoTheReport) {
  const tsupport::SyntheticHiveSpec fx; // AAA/BBB/CCC x 3 dates
  const BuildFixture f = make_build_fixture("failed_cells", fx);
  {
    auto db = SurfaceDb::create(f.db.string());
    ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
    SymbolFitConfig bbb = symbol_config_from_preset(FitPreset::Populate);
    bbb.pin_curve = true;
    bbb.curve.kind = VolCurveKind::LinearVariance;
    ASSERT_TRUE(db->upsert_symbol("BBB", bbb).has_value());
  }

  const auto rep = build_surface_db(build_spec(f, fx));
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->coverage.cells_ok, 6u);     // AAA + CCC on 3 dates
  EXPECT_EQ(rep->coverage.cells_failed, 3u); // BBB on 3 dates

  // The list explains the counter, one entry per failed cell, (date, symbol) asc.
  ASSERT_EQ(rep->coverage.failed_cells.size(), std::size_t{3});
  for (std::size_t i = 0; i < rep->coverage.failed_cells.size(); ++i) {
    const FailedCell &c = rep->coverage.failed_cells[i];
    EXPECT_EQ(c.date, fx.dates[i]);
    EXPECT_EQ(c.symbol, "BBB");
    EXPECT_EQ(c.code, ErrorCode::InvalidArgument);
    // Verbatim from pricer_fitter.cpp — not a re-derivation, not a code name.
    EXPECT_EQ(c.detail, "invalid correctness policy for requested risk surface") << c.detail;
  }

  // ...and the CSV carries every one of them, reason included.
  const fs::path csv = f.root / "failed_cells.csv";
  ASSERT_TRUE(write_build_report_csv(*rep, csv.string()).has_value());
  std::ifstream is(csv.string(), std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("date,symbol,code,detail"), std::string::npos) << text;
  for (const std::string &d : fx.dates) {
    EXPECT_NE(text.find(d + ",BBB,InvalidArgument,\"invalid correctness policy for requested "
                            "risk surface\""),
              std::string::npos)
        << text;
  }
}

// C1, end to end through the exact object the CLI branches on.
//
// This is the second post-FIX-D resume — the run on which the defect appears, and
// only then, because the first resume still re-fits (the pre-FIX-D manifest has
// no fingerprint). The database is FULLY HEALTHY: every date holds every symbol
// that can hold one. BBB fails forever (there is no persisted known-failed state,
// by design), so its three dates are rewritten on every run, and its healthy
// siblings are CARRIED rather than re-fitted. The result is
// `cells_to_fit = 3, cells_ok = 0, cells_carried = 6` — and before the carry
// clause `is_total_fit_failure` read that as a dead build, so the CLI exited 3
// printing "TOTAL FIT FAILURE ... Re-run with the matching --r <rate>". An
// operator who followed that guidance would have re-fitted every surface in the
// database under a wrong rate.
//
// Deliberately NOT a hand-built report: the predicate was already unit-tested on
// synthetic structs and stayed green through the whole defect. This asserts on a
// real `UniversePopulateCoverage` assembled by a real `build_surface_db` resume
// over a failure-bearing database.
TEST(BuildSurfaceDb, ConvergedCarryResumeIsNotATotalFitFailure) {
  const tsupport::SyntheticHiveSpec fx; // AAA/BBB/CCC x 3 dates
  const BuildFixture f = make_build_fixture("carry_exit_code", fx);
  {
    auto db = SurfaceDb::create(f.db.string());
    ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
    // The permanently-failing cell: pin + LinearVariance is refused by the risk
    // pipeline's input validation on every run, forever.
    SymbolFitConfig bbb = symbol_config_from_preset(FitPreset::Populate);
    bbb.pin_curve = true;
    bbb.curve.kind = VolCurveKind::LinearVariance;
    ASSERT_TRUE(db->upsert_symbol("BBB", bbb).has_value());
  }

  // Run 1 populates: AAA and CCC fit on all three dates, BBB fails on all three.
  const auto rep1 = build_surface_db(build_spec(f, fx));
  ASSERT_TRUE(rep1.has_value()) << (rep1 ? "" : rep1.error().to_string());
  ASSERT_EQ(rep1->coverage.cells_ok, 6u);
  ASSERT_EQ(rep1->coverage.cells_failed, 3u);
  EXPECT_FALSE(is_total_fit_failure(*rep1)) << "run 1 fitted 6 cells";

  // Run 2 is the converged steady state. Nothing about the database or the hive
  // changed; the same three dates are rewritten only because BBB is retried.
  const auto rep2 = build_surface_db(build_spec(f, fx));
  ASSERT_TRUE(rep2.has_value()) << (rep2 ? "" : rep2.error().to_string());
  EXPECT_EQ(rep2->coverage.cells_to_fit, 3u) << "the failing cell is retried forever, by design";
  EXPECT_EQ(rep2->coverage.cells_ok, 0u) << "carried cells are deliberately not counted as fits";
  EXPECT_EQ(rep2->coverage.cells_carried, 6u) << "the healthy siblings must be carried";
  EXPECT_EQ(rep2->coverage.cells_refit, 0u);
  EXPECT_EQ(rep2->coverage.cells_failed, 3u);

  // THE assertion. `cells_to_fit > 0 && cells_ok == 0` on a healthy database.
  EXPECT_FALSE(is_total_fit_failure(*rep2))
      << "a converged carry resume must not exit 3 and tell the operator to change --r";
  EXPECT_FALSE(is_total_config_failure(*rep2)) << "the config stage is healthy too";

  // ...and the operator can SEE that carry-over is what made the run quiet (I2):
  // without this counter the report is `ok=0 refit=0 failed=3`, indistinguishable
  // from the dead build the predicate no longer flags.
  const fs::path csv = f.root / "carry_exit_code.csv";
  ASSERT_TRUE(write_build_report_csv(*rep2, csv.string()).has_value());
  std::ifstream is(csv.string(), std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("coverage.cells_carried,6"), std::string::npos) << text;
  // Per-symbol: AAA and CCC each carried on all three dates, and their rows say so.
  EXPECT_NE(text.find("AAA,3,0,0,0,3"), std::string::npos) << text;
  EXPECT_NE(text.find("CCC,3,0,0,0,3"), std::string::npos) << text;

  // FIX-D fix-2 (I-2): the exit code is gone for this shape and must stay gone,
  // but the run is still worth a word. See the dedicated e2e below for both
  // halves; this pins that the very report the CLI branches on trips the warning.
  EXPECT_TRUE(is_carry_masked_fit_failure(*rep2))
      << "the shape the exit code no longer flags must at least be named";
}

// FIX-D fix-2 (I-2), end to end on real reports — the warning that replaced the
// verdict, in both directions.
//
// The exemption `is_total_fit_failure` gained is wider than "a converged
// database": ANY run that carried anything is exempt, including one whose every
// scheduled cell failed systematically. The exit code cannot come back (it would
// be C1 again, and it would tell an operator to re-fit a healthy production
// database under a different --r), so what comes back is a WARNING. For that to
// be worth anything it has to do two things, and both are asserted here on
// `SurfaceDbBuildReport`s a real `build_surface_db` produced:
//
//   FIRES on the ambiguous shape — nothing fitted, something failed, something
//   carried. Run 2 below is exactly that.
//   SILENT on a genuinely converged database — one with nothing left to schedule.
//   Run 3 below is that, reached the way an operator actually reaches it: by
//   disabling the name that fails forever.
TEST(BuildSurfaceDb, CarryMaskedFitFailureFiresOnTheAmbiguousShapeAndNotOnAConvergedDb) {
  const tsupport::SyntheticHiveSpec fx; // AAA/BBB/CCC x 3 dates
  const BuildFixture f = make_build_fixture("carry_masked_warn", fx);
  {
    auto db = SurfaceDb::create(f.db.string());
    ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
    // BBB fails forever: pin + LinearVariance is refused by the risk pipeline's
    // input validation on every run.
    SymbolFitConfig bbb = symbol_config_from_preset(FitPreset::Populate);
    bbb.pin_curve = true;
    bbb.curve.kind = VolCurveKind::LinearVariance;
    ASSERT_TRUE(db->upsert_symbol("BBB", bbb).has_value());
  }

  // Run 1 populates: 6 cells fit, 3 fail. Plenty fitted, so nothing to warn about.
  const auto rep1 = build_surface_db(build_spec(f, fx));
  ASSERT_TRUE(rep1.has_value()) << (rep1 ? "" : rep1.error().to_string());
  ASSERT_EQ(rep1->coverage.cells_ok, 6u);
  EXPECT_FALSE(is_carry_masked_fit_failure(*rep1)) << "a run that fitted 6 cells is legible";

  // Run 2 is the ambiguous shape: ok=0, failed=3, carried=6. Exit stays 0 and the
  // warning is the only thing that distinguishes it from "everything I scheduled
  // just died beside a healthy carried population".
  const auto rep2 = build_surface_db(build_spec(f, fx));
  ASSERT_TRUE(rep2.has_value()) << (rep2 ? "" : rep2.error().to_string());
  ASSERT_EQ(rep2->coverage.cells_ok, 0u);
  ASSERT_EQ(rep2->coverage.cells_failed, 3u);
  ASSERT_EQ(rep2->coverage.cells_carried, 6u);
  EXPECT_TRUE(is_carry_masked_fit_failure(*rep2))
      << "the shape the carry exemption admits must be named on stderr";
  EXPECT_FALSE(is_total_fit_failure(*rep2)) << "and must NOT come back as an exit code";
  EXPECT_FALSE(is_total_config_failure(*rep2));

  // The operator acts on the warning: BBB is the name failing every run, so it is
  // switched off (its already-stored surfaces, if any, are preserved by FIX-E).
  {
    auto db = SurfaceDb::open(f.db.string());
    ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
    auto bbb = db->symbol_config("BBB");
    ASSERT_TRUE(bbb.has_value());
    SymbolFitConfig off = *bbb;
    off.enabled = false;
    ASSERT_TRUE(db->upsert_symbol("BBB", off).has_value());
  }

  // Run 3 is a GENUINELY converged database: no date has anything left to add, so
  // nothing is scheduled, nothing fails, nothing is carried. The warning must go
  // quiet — a line that prints forever regardless of state is not a signal.
  const auto rep3 = build_surface_db(build_spec(f, fx));
  ASSERT_TRUE(rep3.has_value()) << (rep3 ? "" : rep3.error().to_string());
  EXPECT_EQ(rep3->coverage.cells_to_fit, 0u) << "the disabled name is no longer scheduled";
  EXPECT_EQ(rep3->coverage.cells_failed, 0u);
  EXPECT_EQ(rep3->coverage.cells_carried, 0u) << "a skipped-complete date carries nothing";
  EXPECT_EQ(rep3->coverage.dates_skipped_complete, 3u);
  EXPECT_FALSE(is_carry_masked_fit_failure(*rep3))
      << "a converged database must not warn on every run";
  EXPECT_FALSE(is_total_fit_failure(*rep3));
  EXPECT_FALSE(is_total_config_failure(*rep3))
      << "AAA and CCC are still enabled, so the database is not dead";
}

// The display cap. 51 symbols x 17 dates is 867 cells; a wholesale failure must
// print a bounded sample, and the truncation must be COUNTED — a capped list with
// a zero elided count would read as "that was all of them". Same contract as
// verify_db's failures / n_failures_elided, unit-tested on a hand-built report so
// it is pinned independently of any fit running.
[[nodiscard]] SurfaceDbBuildReport report_with_failed_cells(std::size_t n) {
  SurfaceDbBuildReport r;
  r.coverage.cells_failed = static_cast<std::uint32_t>(n);
  for (std::size_t i = 0; i < n; ++i) {
    r.coverage.failed_cells.push_back(
        FailedCell{"2026-07-01", "S" + std::to_string(i), ErrorCode::Unavailable, "why"});
  }
  return r;
}

TEST(SurfaceDbBuildFailedCellCap, CapsTheListAndCountsWhatItLeftOut) {
  const SurfaceDbBuildReport r = report_with_failed_cells(5);

  // Under the cap: everything shown, nothing elided.
  const ReportedFailedCells all = reported_failed_cells(r, 5);
  EXPECT_EQ(all.reported.size(), std::size_t{5});
  EXPECT_EQ(all.n_elided, std::size_t{0});

  // Over the cap: the first `cap` rows (keeping the deterministic order) and an
  // EXACT count of the rest. reported + elided always == the full list.
  const ReportedFailedCells capped = reported_failed_cells(r, 2);
  ASSERT_EQ(capped.reported.size(), std::size_t{2});
  EXPECT_EQ(capped.n_elided, std::size_t{3});
  EXPECT_EQ(capped.reported.size() + capped.n_elided, r.coverage.failed_cells.size());
  EXPECT_EQ(capped.reported[0].symbol, "S0");
  EXPECT_EQ(capped.reported[1].symbol, "S1");
  EXPECT_EQ(capped.reported[0].detail, "why"); // the reason survives the cap

  // A cap of 0 retains no detail at all and elides everything — the counters
  // still tell the truth (verify_db's max_reported_failures == 0 semantics).
  const ReportedFailedCells none = reported_failed_cells(r, 0);
  EXPECT_TRUE(none.reported.empty());
  EXPECT_EQ(none.n_elided, std::size_t{5});

  // An empty list is not a truncation.
  const SurfaceDbBuildReport clean;
  const ReportedFailedCells nothing = reported_failed_cells(clean);
  EXPECT_TRUE(nothing.reported.empty());
  EXPECT_EQ(nothing.n_elided, std::size_t{0});

  // The default cap is what the CLI prints without --max-failures.
  const SurfaceDbBuildReport big =
      report_with_failed_cells(kSurfaceDbBuildMaxReportedFailedCells + 7);
  const ReportedFailedCells defaulted = reported_failed_cells(big);
  EXPECT_EQ(defaulted.reported.size(), kSurfaceDbBuildMaxReportedFailedCells);
  EXPECT_EQ(defaulted.n_elided, std::size_t{7});
}

// The CSV is the artifact an operator greps, so it is NOT subject to the display
// cap: every failed cell appears, including the ones the terminal elides.
TEST(SurfaceDbBuildFailedCellCap, ReportCsvKeepsTheCellsTheTerminalElides) {
  const SurfaceDbBuildReport r =
      report_with_failed_cells(kSurfaceDbBuildMaxReportedFailedCells + 3);
  ASSERT_EQ(reported_failed_cells(r).n_elided, std::size_t{3}); // the terminal drops 3

  const fs::path csv = fresh_dir("failed_cells_csv") / "report.csv";
  fs::create_directories(csv.parent_path());
  ASSERT_TRUE(write_build_report_csv(r, csv.string()).has_value());
  std::ifstream is(csv.string(), std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
  for (std::size_t i = 0; i < r.coverage.failed_cells.size(); ++i) {
    EXPECT_NE(text.find(",S" + std::to_string(i) + ",Unavailable,\"why\""), std::string::npos)
        << "row " << i << " missing from the CSV";
  }
}

// ── is_total_fit_failure — the CLI's exit-code decision ─────────────────────
//
// The predicate the build CLI uses to turn "the build ran and produced nothing"
// into a NON-zero exit. Unit-tested directly on hand-built reports so the three
// shapes it must distinguish are pinned independently of any fit running.

// A report with the coverage counters the predicate reads set explicitly; every
// other field stays default (the predicate must not depend on them).
[[nodiscard]] SurfaceDbBuildReport coverage_report(std::uint32_t to_fit, std::uint32_t ok,
                                                   std::uint32_t failed) {
  SurfaceDbBuildReport r;
  r.coverage.cells_loaded = to_fit;
  r.coverage.cells_to_fit = to_fit;
  r.coverage.cells_ok = ok;
  r.coverage.cells_failed = failed;
  return r;
}

// Work was scheduled and NOT ONE cell fitted — the carry-mismatch signature. This
// is the only shape that may fail the build.
TEST(SurfaceDbTotalFitFailure, TotalFailureIsFailure) {
  EXPECT_TRUE(is_total_fit_failure(coverage_report(9u, 0u, 9u)));
}

// Partial coverage is NORMAL in production (real hives carry unfittable boards).
// Widening the predicate to "any failure" would red-flag every healthy universe
// build, so this must stay false even when most cells failed.
TEST(SurfaceDbTotalFitFailure, PartialFailureIsNotFailure) {
  EXPECT_FALSE(is_total_fit_failure(coverage_report(9u, 5u, 4u)));
  EXPECT_FALSE(is_total_fit_failure(coverage_report(9u, 1u, 8u))); // even 1/9 is not "nothing"
}

// Nothing to do — the RESUME path over an already-complete db (and the un-pulled
// empty window). cells_ok is legitimately 0 because no cell was scheduled. The
// build's convergence guarantee is "a re-run fits zero", so this MUST stay green.
TEST(SurfaceDbTotalFitFailure, NothingToDoIsNotFailure) {
  EXPECT_FALSE(is_total_fit_failure(coverage_report(0u, 0u, 0u)));
  SurfaceDbBuildReport resumed = coverage_report(0u, 0u, 0u);
  resumed.coverage.cells_loaded = 9u; // 9 cells loaded, all already present
  resumed.coverage.cells_already_present = 9u;
  resumed.coverage.dates_skipped_complete = 3u;
  EXPECT_FALSE(is_total_fit_failure(resumed));
}

// A clean build is obviously not a failure.
TEST(SurfaceDbTotalFitFailure, HealthyBuildIsNotFailure) {
  EXPECT_FALSE(is_total_fit_failure(coverage_report(9u, 9u, 0u)));
}

// FIX-D's converged steady state, and the shape that made this predicate misfire:
// a date holding permanently-failing cells is rewritten every run, its failures
// are retried forever (by design), and its healthy siblings are CARRIED rather
// than re-fitted -- so cells_ok is legitimately 0 on a fully healthy database.
//
// This is the production shape exactly: 3 permanently-failing cells, 147 carried
// siblings. Before the carry clause this returned true and the CLI told the
// operator to re-run with a different --r, which would have invalidated every
// surface in the database.
TEST(SurfaceDbTotalFitFailure, CarriedOnlyResumeIsNotFailure) {
  SurfaceDbBuildReport carried = coverage_report(3u, 0u, 3u);
  carried.coverage.cells_carried = 147u;
  carried.coverage.cells_loaded = 150u;
  carried.coverage.dates_written = 3u;
  EXPECT_FALSE(is_total_fit_failure(carried))
      << "the converged carry steady state is healthy, not a total fit failure";

  // One carried cell is enough: the database is not empty.
  SurfaceDbBuildReport minimal = coverage_report(1u, 0u, 1u);
  minimal.coverage.cells_carried = 1u;
  EXPECT_FALSE(is_total_fit_failure(minimal));

  // But with NOTHING carried and nothing fitted it is still the real failure.
  SurfaceDbBuildReport dead = coverage_report(3u, 0u, 3u);
  dead.coverage.cells_carried = 0u;
  EXPECT_TRUE(is_total_fit_failure(dead));
}

// ── is_carry_masked_fit_failure — the signal the exemption above gave up ─────
//
// FIX-D fix-2 (I-2). The carry clause exempts ANY run that carried anything, not
// only a converged database. So a run whose every SCHEDULED cell failed
// systematically, beside a healthy carried population, exits 0 where before
// carry-over it exited 3. That shape is genuinely ambiguous between "converged,
// one permanently-bad cell" and "everything I scheduled just died", and no
// counter can separate them — so the tool WARNS and still exits 0. Restoring the
// exit code here would recreate C1 exactly, because a healthy production database
// matches this shape on every run.

// The reviewer's counter-example, and the production shape, are the SAME shape —
// which is the finding. Both must fire the warning.
TEST(SurfaceDbCarryMaskedFitFailure, FiresOnTheAmbiguousShape) {
  // Add one ticker to a converged 1030-name universe: every date is rewritten,
  // ~257k healthy cells are carried, ~250 new cells are scheduled, and all 250
  // die for a systematic reason that is not the carry rate.
  SurfaceDbBuildReport systematic = coverage_report(250u, 0u, 250u);
  systematic.coverage.cells_carried = 257'000u;
  EXPECT_TRUE(is_carry_masked_fit_failure(systematic));
  EXPECT_FALSE(is_total_fit_failure(systematic))
      << "the exit code must stay 0 — this is the shape C1 exists to protect";

  // The converged steady state (3 permanently-failing cells, 147 carried
  // siblings) is indistinguishable from it by construction, so it warns too. That
  // is not a false positive: the warning claims only that the run is one of the
  // two and points at the failed_cell reasons, which DO separate them.
  SurfaceDbBuildReport converged = coverage_report(3u, 0u, 3u);
  converged.coverage.cells_carried = 147u;
  EXPECT_TRUE(is_carry_masked_fit_failure(converged));
  EXPECT_FALSE(is_total_fit_failure(converged));
}

// ...and it must be SILENT on a genuinely converged database, or it is noise. A
// converged resume has nothing left to schedule: every date is
// dates_skipped_complete, nothing is fitted, nothing fails, nothing is carried
// (carrying only happens on a date being rewritten).
TEST(SurfaceDbCarryMaskedFitFailure, SilentOnAGenuinelyConvergedResume) {
  SurfaceDbBuildReport resumed = coverage_report(0u, 0u, 0u);
  resumed.coverage.cells_loaded = 150u;
  resumed.coverage.cells_already_present = 150u;
  resumed.coverage.dates_skipped_complete = 3u;
  EXPECT_FALSE(is_carry_masked_fit_failure(resumed))
      << "a converged database must not warn on every run";
  EXPECT_FALSE(is_total_fit_failure(resumed));

  // A rewrite that carried and lost NOTHING is equally quiet: no failure, no
  // ambiguity to report.
  SurfaceDbBuildReport clean_carry = coverage_report(0u, 0u, 0u);
  clean_carry.coverage.cells_carried = 147u;
  clean_carry.coverage.dates_written = 3u;
  EXPECT_FALSE(is_carry_masked_fit_failure(clean_carry));

  // And an empty window says nothing at all.
  EXPECT_FALSE(is_carry_masked_fit_failure(SurfaceDbBuildReport{}));
}

// Anything fitted at all makes the run legible on its own: partial coverage is
// normal production output and is not what this warning is about.
TEST(SurfaceDbCarryMaskedFitFailure, SilentWhenAnythingFitted) {
  SurfaceDbBuildReport partial = coverage_report(9u, 1u, 8u);
  partial.coverage.cells_carried = 147u;
  EXPECT_FALSE(is_carry_masked_fit_failure(partial));
}

// The warning and the exit code are DISJOINT: the predicate above needs
// `cells_carried == 0`, this one needs `> 0`. The CLI can never emit both, so a
// dead build gets the verdict and never the hedge.
TEST(SurfaceDbCarryMaskedFitFailure, NeverOverlapsTheExitCode) {
  const SurfaceDbBuildReport dead = coverage_report(3u, 0u, 3u); // cells_carried == 0
  EXPECT_TRUE(is_total_fit_failure(dead));
  EXPECT_FALSE(is_carry_masked_fit_failure(dead))
      << "a genuine total fit failure must get the exit code, not the warning";
}

// COUNTER CHOICE, pinned. `cells_carried` is what grants the exit-3 exemption, so
// it is what the warning tracks. FIX-E's `cells_carried_disabled` — preserved
// surfaces of a symbol the operator switched OFF — grants no exemption, so such a
// run is still a TOTAL FIT FAILURE and must not be softened into a warning.
TEST(SurfaceDbCarryMaskedFitFailure, DisabledCarryIsNotHealthyCarry) {
  SurfaceDbBuildReport preserved = coverage_report(3u, 0u, 3u);
  preserved.coverage.cells_carried = 0u;
  preserved.coverage.cells_carried_disabled = 9u;
  EXPECT_TRUE(is_total_fit_failure(preserved))
      << "preserved bytes of a switched-off name are not proof the run produced anything";
  EXPECT_FALSE(is_carry_masked_fit_failure(preserved))
      << "the warning must not replace the exit code for a shape that is still dead";
}

// ── is_total_config_failure — the SAME trap one stage earlier ───────────────
//
// `is_total_fit_failure` only sees cells that were SCHEDULED. When config
// selection fails for every symbol, every config is stored disabled, nothing is
// ever scheduled (`cells_to_fit == 0`), and the fit predicate reads that as the
// healthy "nothing to do" resume. The build exits 0 over a database that has no
// enabled symbol and will never have a surface.

// A report with the config counters the predicate reads set explicitly. The three
// dispositions partition the symbols the stage saw (`n_configured +
// n_skipped_existing + n_disabled_failed == n_symbols`), so they are set as a set.
[[nodiscard]] SurfaceDbBuildReport config_report(std::uint32_t configured,
                                                 std::uint32_t skipped_existing,
                                                 std::uint32_t disabled_failed) {
  SurfaceDbBuildReport r;
  r.config.n_symbols = configured + skipped_existing + disabled_failed;
  r.config.n_configured = configured;
  r.config.n_skipped_existing = skipped_existing;
  r.config.n_disabled_failed = disabled_failed;
  return r;
}

// EVERY symbol we tried to configure was disabled by a selection failure, and the
// run got nothing out of it. Same class of silent green as a total fit failure,
// and it reuses the same exit.
TEST(SurfaceDbTotalConfigFailure, EveryConfigDisabledIsFailure) {
  EXPECT_TRUE(is_total_config_failure(config_report(0u, 0u, 3u)));
  // ...and the fit predicate cannot see it: nothing was ever scheduled.
  EXPECT_FALSE(is_total_fit_failure(config_report(0u, 0u, 3u)));
}

// PARTIAL selection failure is normal production output — a real universe carries
// names whose board cannot pin a curve, and they are fail-closed disabled while
// the rest build. Exactly the rule cd9b491 established for the fit stage.
TEST(SurfaceDbTotalConfigFailure, PartialConfigFailureIsNotFailure) {
  EXPECT_FALSE(is_total_config_failure(config_report(2u, 0u, 1u)));
  EXPECT_FALSE(is_total_config_failure(config_report(1u, 0u, 8u))); // even 1/9 is not "nothing"
}

// The RESUME path: every symbol was already configured, so the stage disabled
// nothing and attempted nothing. This is the convergence guarantee and must stay
// green — it is the shape the new predicate has to be distinguishable from.
TEST(SurfaceDbTotalConfigFailure, NothingToDoResumeIsNotFailure) {
  EXPECT_FALSE(is_total_config_failure(config_report(0u, 3u, 0u)));
  SurfaceDbBuildReport resumed = config_report(0u, 3u, 0u);
  resumed.coverage.cells_loaded = 9u;
  resumed.coverage.cells_already_present = 9u;
  resumed.coverage.dates_skipped_complete = 3u;
  EXPECT_FALSE(is_total_config_failure(resumed));
  // An empty window saw no symbols at all — nothing was attempted, nothing failed.
  EXPECT_FALSE(is_total_config_failure(config_report(0u, 0u, 0u)));
}

// A clean build configures everything and is obviously not a failure.
TEST(SurfaceDbTotalConfigFailure, HealthyBuildIsNotFailure) {
  EXPECT_FALSE(is_total_config_failure(config_report(3u, 0u, 0u)));
}

// FIX-C-2 changed one exit-code shape in the OTHER direction, so pin it: new
// names failing selection beside symbols that were already configured and ENABLED
// but produced no cell this run (a converged resume — `cells_ok == 0` because
// there was nothing to do, not because nothing works).
//
// The old predicate read only this run's fresh verdicts, saw `n_configured == 0`,
// and called that a total config failure — exit 3 over a database serving five
// symbols perfectly well. Reading the standing state gets it right: five enabled,
// so the database is alive and the three new failures are partial. Exit 0.
TEST(SurfaceDbTotalConfigFailure, NewNamesFailingBesideAConvergedResumeIsNotFailure) {
  SurfaceDbBuildReport r = config_report(0u, 5u, 3u); // 5 skipped (all ENABLED), 3 newly disabled
  r.coverage.cells_ok = 0u;                          // converged: nothing left to fit
  r.coverage.cells_already_present = 15u;
  r.coverage.dates_skipped_complete = 3u;
  EXPECT_FALSE(is_total_config_failure(r));

  // The distinguishing bit is whether those five are actually enabled. Mark them
  // disabled and the same counters become the dead database, and exit 3.
  r.config.n_disabled_existing = 5u;
  EXPECT_TRUE(is_total_config_failure(r));
}

// FIX-C-2. The SAME dead database, one run later. Every symbol is stored disabled
// and therefore SKIPPED, so this run's fresh-failure counter is 0 and the shape is
// byte-identical to the healthy nothing-to-do resume above — except that not one
// symbol in the database is enabled and no cell can ever be scheduled. The verdict
// must not depend on which run you are on.
TEST(SurfaceDbTotalConfigFailure, ResumeOverAnAllDisabledDbIsFailure) {
  SurfaceDbBuildReport r = config_report(0u, 3u, 0u); // every symbol skip-existing
  r.config.n_disabled_existing = 3u;                  // ...and every one of them disabled
  r.config.failed_symbols = {"AAA", "BBB", "CCC"};
  EXPECT_TRUE(is_total_config_failure(r));
  // Still invisible to the fit predicate — nothing was ever scheduled.
  EXPECT_FALSE(is_total_fit_failure(r));

  // One enabled name among them is a PARTIALLY disabled database: a lost symbol,
  // not a dead build. This is the production 50-of-51 shape and must stay green.
  SurfaceDbBuildReport partial = config_report(0u, 3u, 0u);
  partial.config.n_disabled_existing = 1u;
  partial.config.failed_symbols = {"CCC"};
  EXPECT_FALSE(is_total_config_failure(partial));
}

// FIX-D fix-1: the C1 audit's verdict on THIS predicate, pinned.
//
// `is_total_config_failure` carries the identical `cells_ok == 0` conjunct that
// FIX-D turned into a false verdict one stage down, so it now carries the carry
// clause too. The trap is unreachable end to end (a carried cell is by
// construction proof that some symbol is enabled — see the reachability argument
// in surface_db_build.cpp), which is exactly why it is pinned here on a synthetic
// report: nothing else can reach the shape, and without a test the clause would
// be silently deletable.
TEST(SurfaceDbTotalConfigFailure, CarriedCellsAreProofTheDatabaseIsNotDead) {
  SurfaceDbBuildReport dead = config_report(0u, 3u, 0u); // every symbol skip-existing
  dead.config.n_disabled_existing = 3u;                  // ...and every one disabled
  ASSERT_TRUE(is_total_config_failure(dead)) << "the dead-database baseline must still fire";

  // Same config counters, but this run re-emitted stored surfaces. Whatever the
  // config stage says, the database demonstrably holds and served surfaces — the
  // same class of evidence as cells_ok > 0, which has always been an exit.
  SurfaceDbBuildReport carried = dead;
  carried.coverage.cells_carried = 147u;
  EXPECT_FALSE(is_total_config_failure(carried));

  // And the two predicates agree on that evidence: neither may call this dead.
  carried.coverage.cells_to_fit = 3u;
  carried.coverage.cells_failed = 3u;
  EXPECT_FALSE(is_total_fit_failure(carried));
  EXPECT_FALSE(is_total_config_failure(carried));
}

// The one shape that must NOT be swept in: symbols that were already configured
// went on to fit successfully, and only the newly-seen names failed selection. The
// run produced surfaces, so it is partial, not dead.
TEST(SurfaceDbTotalConfigFailure, NewNamesFailingBesideProductiveFitsIsNotFailure) {
  SurfaceDbBuildReport r = config_report(0u, 5u, 2u);
  r.coverage.cells_to_fit = 5u;
  r.coverage.cells_ok = 5u;
  EXPECT_FALSE(is_total_config_failure(r));
}

// The config trap end to end at the library level: a hive whose every board is
// REAL but unselectable (one quote per cell — a single strike cannot pin a curve
// family). Every symbol is stored disabled, so nothing is ever scheduled, the
// build returns Ok, and `is_total_fit_failure` sees the healthy nothing-to-do
// resume. Only the config predicate can tell this apart from a converged re-run.
TEST(BuildSurfaceDb, EverySymbolFailingSelectionIsTotalConfigFailure) {
  tsupport::SyntheticHiveSpec fx;
  // Two quotes per cell: enough for the LOADER to accept every board (all three
  // dates load, zero load errors — the config stage really does see all three
  // symbols), and far too few for `select_fit_policy` to pin a curve family.
  fx.max_rows_per_cell = 2;
  const BuildFixture f = make_build_fixture("all_configs_disabled", fx);

  const auto rep = build_surface_db(build_spec(f, fx));
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string()); // still Ok — the trap
  EXPECT_EQ(rep->n_dates_loaded, std::size_t{3}); // the boards are REAL, not quarantined
  EXPECT_EQ(rep->n_load_errors, std::size_t{0});
  EXPECT_EQ(rep->config.n_symbols, 3u);
  EXPECT_EQ(rep->config.n_configured, 0u);
  EXPECT_EQ(rep->config.n_disabled_failed, 3u);
  EXPECT_EQ(rep->config.failed_symbols.size(), std::size_t{3}); // each one named
  EXPECT_EQ(rep->coverage.cells_to_fit, 0u);                    // nothing scheduled...
  EXPECT_EQ(rep->coverage.cells_ok, 0u);                        // ...so nothing fitted

  EXPECT_FALSE(is_total_fit_failure(*rep)) << "the fit predicate cannot see this — that IS the bug";
  EXPECT_TRUE(is_total_config_failure(*rep));

  // The database it left behind: created, no enabled symbol, no partition. This is
  // the thing that used to pass a build AND an unguarded verify.
  auto db = SurfaceDb::open(f.db.string());
  ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
  EXPECT_EQ(db->partitions().size(), std::size_t{0});
  for (const std::string &s : db->symbols()) {
    const auto cfg = db->symbol_config(s);
    ASSERT_TRUE(cfg.has_value()) << (cfg ? "" : cfg.error().to_string());
    EXPECT_FALSE(cfg->enabled) << s;
  }

  // Same hive, full boards: the predicate stays quiet on a healthy build.
  const tsupport::SyntheticHiveSpec full;
  const BuildFixture g = make_build_fixture("all_configs_ok", full);
  const auto good = build_surface_db(build_spec(g, full));
  ASSERT_TRUE(good.has_value()) << (good ? "" : good.error().to_string());
  EXPECT_EQ(good->config.n_configured, 3u);
  EXPECT_FALSE(is_total_config_failure(*good));
}

// The production trap, end to end at the library level: the synthetic hive is
// priced at a NON-ZERO rate (SyntheticHiveSpec::r == 0.03), so building it with
// the CLI's old hard-wired r = 0.0 fails every single fit while the build itself
// still returns Ok — exactly the silent green exit `--r` and this predicate close.
// The same build with the MATCHING r fits everything.
TEST(BuildSurfaceDb, ZeroCarryAgainstNonZeroHiveIsTotalFailure) {
  const tsupport::SyntheticHiveSpec fx; // r = 0.03
  ASSERT_GT(fx.r, 0.0) << "fixture must embed a non-zero carry for this test to mean anything";
  const BuildFixture f = make_build_fixture("carry_mismatch", fx);

  SurfaceDbBuildSpec spec = build_spec(f, fx);
  spec.hive.r = 0.0; // the pre---r CLI default: wrong for this hive
  const auto bad = build_surface_db(spec);
  ASSERT_TRUE(bad.has_value()) << (bad ? "" : bad.error().to_string()); // still Ok — the trap
  EXPECT_GT(bad->coverage.cells_to_fit, 0u);
  EXPECT_EQ(bad->coverage.cells_ok, 0u);
  EXPECT_TRUE(is_total_fit_failure(*bad));

  // Same hive, same db root, correct carry: the build converges.
  const BuildFixture g = make_build_fixture("carry_match", fx);
  const auto good = build_surface_db(build_spec(g, fx)); // spec.hive.r == fx.r
  ASSERT_TRUE(good.has_value()) << (good ? "" : good.error().to_string());
  EXPECT_EQ(good->coverage.cells_ok, 9u);
  EXPECT_FALSE(is_total_fit_failure(*good));
}

// ── FIX-C-1: a punctuated ticker must reach the fitter ──────────────────────
//
// A production 51-name build configured 50 symbols and disabled exactly one:
// BRK.B, the only ticker in the universe carrying punctuation. Its 1,838 quotes
// (103 strikes, 14 expiries) were in the hive and in memory the whole time — the
// loader simply derived the underlier's name TWO ways that agree for every other
// ticker: `frame.uid` from the hive's `underlying` column ("BRK.B") and every
// `QuoteRow::uid` from the OSI root ("BRKB"). `intern_ticker` compares exact
// bytes, so `data_install` created two underliers, filed every quote under the
// root, and returned the handle to the EMPTY dotted one; config selection then
// fail-closed-disabled a symbol whose board was complete.
//
// The fixture reproduces the real divergence rather than asserting on strings:
// `underlying` says `BRK.B` while every `symbol` says `BRKB  260729C00100000`,
// exactly as `pull_opra_hive.py` writes it.

// The loader seam itself: ONE requested name yields ONE uid, and it is the dotted
// universe spelling the rest of the pipeline keys on — not the wire root.
TEST(BuildSurfaceDb, PunctuatedTickerLoadsAsOneUnderlier) {
  tsupport::SyntheticHiveSpec fx;
  fx.symbols = {"AAA", "BRK.B"};
  const BuildFixture f = make_build_fixture("dotted_load", fx);

  const std::vector<CorpusBoard> boards = load_fixture_hive_boards(f, fx);
  ASSERT_EQ(boards.size(), std::size_t{6}); // 2 symbols x 3 dates

  bool saw_dotted = false;
  for (const CorpusBoard &b : boards) {
    if (b.symbol != "BRK.B") {
      continue;
    }
    saw_dotted = true;
    EXPECT_EQ(b.frame.uid, "BRK.B") << "the frame must carry the universe spelling";
    // The assertion that actually bites: the frame claims exactly ONE underlier.
    // Pre-fix this was {"BRK.B", "BRKB"} — a single-name board naming two.
    ASSERT_EQ(b.frame.uid_strs.size(), std::size_t{1})
        << "frame.uid and the row uids disagree — the BRK.B split";
    EXPECT_EQ(b.frame.uid_strs.front(), "BRK.B");
    ASSERT_FALSE(b.frame.rows.empty());
    for (const QuoteRow &row : b.frame.rows) {
      EXPECT_EQ(row.uid, "BRK.B");
    }
  }
  EXPECT_TRUE(saw_dotted) << "fixture did not produce a BRK.B board";
}

// End to end: the punctuated symbol is CONFIGURED (enabled) and FITTED, and its
// surface is mappable out of the reopened database. Asserting on the config
// verdict and on real partition bytes, not on a string match.
TEST(BuildSurfaceDb, PunctuatedTickerIsConfiguredAndFitted) {
  tsupport::SyntheticHiveSpec fx;
  fx.symbols = {"AAA", "BRK.B"};
  const BuildFixture f = make_build_fixture("dotted_e2e", fx);

  const auto rep = build_surface_db(build_spec(f, fx));
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->n_load_errors, std::size_t{0});
  EXPECT_EQ(rep->config.n_symbols, 2u);
  EXPECT_EQ(rep->config.n_configured, 2u);   // BOTH names, not 1 of 2
  EXPECT_EQ(rep->config.n_disabled_failed, 0u);
  EXPECT_TRUE(rep->config.failed_symbols.empty())
      << "a punctuated ticker was disabled: " << rep->config.failed_symbols.front();
  EXPECT_EQ(rep->coverage.cells_ok, 6u); // 2 symbols x 3 dates, all fitted

  // The manifest holds it ENABLED under its dotted spelling.
  auto db = SurfaceDb::open(f.db.string());
  ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
  const auto cfg = db->symbol_config("BRK.B");
  ASSERT_TRUE(cfg.has_value()) << (cfg ? "" : cfg.error().to_string());
  EXPECT_TRUE(cfg->enabled);

  // ...and the fitter really produced a surface for it on every date.
  for (const std::string &date : fx.dates) {
    EXPECT_TRUE(db->map_surface(date, "BRK.B").has_value()) << date;
  }
}

// The other side of the C-1 rule, and the guard that can actually fire. Keying
// rows by the `underlying` column is sound only while that column names the same
// underlier the row's OSI symbol does. Dots are a legitimate difference (above);
// anything else is TWO underliers. The live way to manufacture that is the OCC
// adjusted-deliverable class — `pull_opra_hive.py` strips a trailing digit from
// the root before mapping, so an `AAA1` row can end up carrying
// `underlying = "AAA"`. Merging those into the vanilla chain at the same strike is
// a silent pricing error, strictly worse than the lost symbol C-1 fixed, so the
// loader refuses the cell loudly and it lands in `n_load_errors`.
TEST(BuildSurfaceDb, RootDisagreeingBeyondPunctuationIsALoudLoadError) {
  tsupport::SyntheticHiveSpec fx;
  fx.symbols = {"AAA", "BBB"};
  fx.adjusted_root_symbols = {"BBB"}; // BBB's contracts trade as "BBB1"
  const BuildFixture f = make_build_fixture("adjusted_root", fx);

  const auto rep = build_surface_db(build_spec(f, fx));
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string()); // one bad cell never aborts
  // Loud and correctly classified: a real defect, not a sparse-universe hole.
  EXPECT_EQ(rep->n_load_errors, std::size_t{3}); // BBB on each of the 3 dates
  EXPECT_EQ(rep->n_coverage_holes, std::size_t{0});
  // BBB never reaches config (no board at all), and is NOT silently folded into
  // some other underlier's chains.
  EXPECT_EQ(rep->config.n_symbols, 1u);
  EXPECT_EQ(rep->config.n_configured, 1u);
  EXPECT_EQ(rep->coverage.cells_ok, 3u); // AAA's three dates, unaffected

  auto db = SurfaceDb::open(f.db.string());
  ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
  EXPECT_FALSE(db->symbol_config("BBB").has_value());
  EXPECT_TRUE(db->symbol_config("AAA").has_value());
  for (const std::string &date : fx.dates) {
    EXPECT_TRUE(db->map_surface(date, "AAA").has_value()) << date;
  }
}

// ── FIX-C-2: a standing disabled symbol is named on EVERY run ───────────────
//
// The first build over a database names its casualty (`config.failed_symbols`).
// Every build after it used to name nothing: the disabled config is a stored
// config, so it became `n_skipped_existing`, its dates reported
// `dates_skipped_complete`, and the rerun printed an all-green report while the
// symbol stayed permanently absent from every partition. The one run that named
// it was the only run that ever would.
TEST(BuildSurfaceDb, ResumeStillNamesTheDisabledSymbols) {
  const tsupport::SyntheticHiveSpec fx; // AAA/BBB/CCC x 3 dates
  const BuildFixture f = make_build_fixture("resume_names_disabled", fx);
  const SurfaceDbBuildSpec spec = build_spec(f, fx);

  // Store a DISABLED CCC through the real fail-closed path (gut its boards to a
  // single quote so selection cannot run), exactly as production stored BRK.B.
  {
    auto db = SurfaceDb::create(f.db.string());
    ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
    std::vector<CorpusBoard> gutted = load_fixture_hive_boards(f, fx);
    for (CorpusBoard &b : gutted) {
      if (b.symbol == "CCC") {
        ASSERT_FALSE(b.frame.rows.empty());
        b.frame.rows.resize(1);
      }
    }
    const auto seeded = generate_symbol_configs(*db, gutted, AutoConfigSpec{});
    ASSERT_TRUE(seeded.has_value()) << (seeded ? "" : seeded.error().to_string());
    ASSERT_EQ(seeded->n_disabled_failed, 1u);
    ASSERT_EQ(seeded->n_disabled_existing, 0u); // freshly failed, not carried
  }

  ASSERT_TRUE(build_surface_db(spec).has_value()); // first build over the seeded db

  // The RERUN. Nothing failed on this run; CCC is skipped as already-configured.
  // It must still be named, and counted as a standing disable.
  const auto rep2 = build_surface_db(spec);
  ASSERT_TRUE(rep2.has_value()) << (rep2 ? "" : rep2.error().to_string());
  EXPECT_EQ(rep2->config.n_skipped_existing, 3u);
  EXPECT_EQ(rep2->config.n_disabled_failed, 0u); // nothing failed HERE — that is the trap
  EXPECT_EQ(rep2->config.n_disabled_existing, 1u);
  ASSERT_EQ(rep2->config.failed_symbols.size(), std::size_t{1});
  EXPECT_EQ(rep2->config.failed_symbols.front(), "CCC");
  // The partition side of the same story stays all-green, which is why the report
  // has to carry the name: nothing here says a symbol is missing.
  EXPECT_EQ(rep2->coverage.cells_to_fit, 0u);
  EXPECT_EQ(rep2->coverage.dates_skipped_complete, 3u);
  // 50-of-51 is a partial build, not a dead one — the exit code must stay 0.
  EXPECT_FALSE(is_total_config_failure(*rep2));

  // The DURABLE artifact carries the name too: the CSV used to hold the count and
  // never the name, so once the terminal scrollback was gone the evidence was too.
  const fs::path csv = f.root / "resume-report.csv";
  ASSERT_TRUE(write_build_report_csv(*rep2, csv.string()).has_value());
  std::ifstream is(csv.string(), std::ios::binary);
  ASSERT_TRUE(is.good());
  const std::string body((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
  EXPECT_NE(body.find("config.n_disabled_existing,1"), std::string::npos);
  EXPECT_NE(body.find("config_disabled_symbol\nCCC\n"), std::string::npos)
      << "the --report CSV must name the disabled symbol, not just count it";
}

// The C-1/C-2 interaction, and the reason a naming fix alone is not enough: an
// ALREADY-BUILT database carries the disabled config, and skip-existing keeps it
// disabled forever — so a corrected loader can never reach the symbol it fixed.
// `retry_disabled` re-selects exactly the stored disables, leaves enabled configs
// (operator overrides) untouched, and the symbol fits on that same run.
TEST(BuildSurfaceDb, RetryDisabledReattemptsAStoredDisableAndFitsIt) {
  const tsupport::SyntheticHiveSpec fx; // AAA/BBB/CCC x 3 dates
  const BuildFixture f = make_build_fixture("retry_disabled", fx);
  const SurfaceDbBuildSpec spec = build_spec(f, fx);

  // A database already built with CCC stored disabled (the prod-2026-07 shape).
  {
    auto db = SurfaceDb::create(f.db.string());
    ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
    std::vector<CorpusBoard> gutted = load_fixture_hive_boards(f, fx);
    for (CorpusBoard &b : gutted) {
      if (b.symbol == "CCC") {
        b.frame.rows.resize(1);
      }
    }
    ASSERT_TRUE(generate_symbol_configs(*db, gutted, AutoConfigSpec{}).has_value());
  }
  const auto built = build_surface_db(spec);
  ASSERT_TRUE(built.has_value()) << (built ? "" : built.error().to_string());
  ASSERT_EQ(built->coverage.cells_ok, 6u); // AAA + BBB only; CCC is fenced out
  {
    auto db = SurfaceDb::open(f.db.string());
    ASSERT_TRUE(db.has_value());
    EXPECT_FALSE(db->map_surface(fx.dates.front(), "CCC").has_value());
  }

  // Hand-tune AAA so the retry can be shown NOT to be an overwrite.
  double tuned_band_k = 0.0;
  {
    auto db = SurfaceDb::open(f.db.string());
    ASSERT_TRUE(db.has_value());
    const auto aaa = db->symbol_config("AAA");
    ASSERT_TRUE(aaa.has_value());
    SymbolFitConfig edited = *aaa;
    tuned_band_k = edited.band_k + 4.0;
    edited.band_k = tuned_band_k;
    ASSERT_TRUE(db->upsert_symbol("AAA", edited).has_value());
  }

  // The operator's re-attempt, no database deletion involved.
  SurfaceDbBuildSpec retry = spec;
  retry.auto_config.retry_disabled = true;
  const auto rep = build_surface_db(retry);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->config.n_configured, 1u);        // CCC, re-selected
  EXPECT_EQ(rep->config.n_skipped_existing, 2u);  // AAA/BBB untouched
  EXPECT_EQ(rep->config.n_disabled_existing, 0u); // nothing left disabled
  EXPECT_EQ(rep->config.n_disabled_failed, 0u);
  EXPECT_TRUE(rep->config.failed_symbols.empty());
  EXPECT_EQ(rep->coverage.cells_to_fit, 3u); // CCC's three dates, now schedulable
  EXPECT_EQ(rep->coverage.cells_failed, 0u);
  EXPECT_GE(rep->coverage.cells_ok, 3u); // + AAA/BBB re-fit by the same-date rewrite

  auto db = SurfaceDb::open(f.db.string());
  ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
  const auto ccc = db->symbol_config("CCC");
  ASSERT_TRUE(ccc.has_value());
  EXPECT_TRUE(ccc->enabled);
  for (const std::string &date : fx.dates) {
    EXPECT_TRUE(db->map_surface(date, "CCC").has_value()) << date;
  }
  // The enabled neighbour kept the operator's edit: a retry is not an overwrite.
  const auto aaa = db->symbol_config("AAA");
  ASSERT_TRUE(aaa.has_value());
  EXPECT_DOUBLE_EQ(aaa->band_k, tuned_band_k);
}

} // namespace
} // namespace atx::vol
