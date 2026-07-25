// SurfaceDbAdmin suite — proves the CLI-native management/inspection layer
// (atx/vol/surface_db_admin.hpp) answers, from C++ alone, every question the
// retired Python `map_surface` check used to answer:
//
//   describe_db        — counts/bytes agree with what build_surface_db reported.
//   describe_partition — the symbols a partition ACTUALLY carries.
//   describe_symbol    — the stored config, including a fail-closed DISABLED one.
//   query_surface      — a finite iv for a real cell; clean errors for bad input.
//   verify_db          — all-ok on a healthy db; every broken cell IDENTIFIED,
//                        with the failure list capped and the elision counted.
//
// The database under test is a REAL one: the Task 2 synthetic hive fixture
// (tests/support/synthetic_opra_hive.hpp) written to disk, then built through
// `build_surface_db` — the same ingest surface_db_build_test.cpp exercises. The
// broken-database cases damage that real database on disk (unlink a partition
// file; disable a symbol through the real fail-closed path); nothing is mocked.

#include "support/synthetic_opra_hive.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/corpus.hpp"           // CorpusBoard
#include "atx/vol/opra_batch.hpp"       // corpus_board_from_opra
#include "atx/vol/opra_hive.hpp"        // OpraHiveSpec, load_opra_hive
#include "atx/vol/session.hpp"          // FitPreset
#include "atx/vol/surface_db.hpp"       // SurfaceDb
#include "atx/vol/surface_db_admin.hpp" // the unit under test
#include "atx/vol/surface_db_build.hpp" // build_surface_db, generate_symbol_configs
#include "atx/vol/types.hpp"

namespace atx::vol {
namespace {

namespace fs = std::filesystem;
namespace tsupport = atx::vol::testsupport;

[[nodiscard]] fs::path fresh_dir(std::string_view name) {
  fs::path p = fs::temp_directory_path() / ("atx_surface_db_admin_" + std::string(name));
  fs::remove_all(p);
  return p;
}

// A fresh workspace: a synthetic hive-v2 tree plus the db root built from it.
struct AdminFixture {
  fs::path root;
  fs::path hive;
  fs::path db;
  tsupport::SyntheticHiveSpec fx{}; // AAA/BBB/CCC x {07-01, 07-02, 07-06}
};

[[nodiscard]] AdminFixture make_fixture(std::string_view name) {
  AdminFixture f;
  f.root = fresh_dir(name);
  f.hive = f.root / "hive";
  f.db = f.root / "db";
  tsupport::write_synthetic_hive_v2(f.hive, f.fx);
  return f;
}

[[nodiscard]] SurfaceDbBuildSpec build_spec(const AdminFixture &f) {
  SurfaceDbBuildSpec spec;
  spec.db_root = f.db.string();
  spec.hive.root_dir = f.hive.string();
  spec.hive.date_lo = f.fx.dates.front();
  spec.hive.date_hi = f.fx.dates.back();
  spec.hive.symbols = f.fx.symbols;
  spec.hive.r = f.fx.r;
  return spec;
}

// Build the fixture's database end-to-end and return the build report, asserting
// it produced the healthy 3x3 shape every case below assumes.
SurfaceDbBuildReport build_healthy_db(const AdminFixture &f) {
  const auto rep = build_surface_db(build_spec(f));
  EXPECT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  if (!rep) {
    return SurfaceDbBuildReport{};
  }
  EXPECT_EQ(rep->coverage.cells_ok, 9u);
  EXPECT_EQ(rep->coverage.dates_written, 3u);
  return *rep;
}

[[nodiscard]] SurfaceDb open_db(const AdminFixture &f) {
  auto db = SurfaceDb::open(f.db.string());
  EXPECT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
  return std::move(*db);
}

// Load the fixture hive's boards through the real loader (the ingest
// build_surface_db runs) so a case can gut one and drive the fail-closed
// disable path — surface_db_build_test.cpp's technique.
[[nodiscard]] std::vector<CorpusBoard> load_fixture_boards(const AdminFixture &f) {
  OpraHiveSpec spec;
  spec.root_dir = f.hive.string();
  spec.date_lo = f.fx.dates.front();
  spec.date_hi = f.fx.dates.back();
  spec.symbols = f.fx.symbols;
  spec.r = f.fx.r;

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

// The on-disk path of one partition file (the documented db layout:
// <root>/partitions/<KEY>.atxvsa). Used to DAMAGE a real database.
[[nodiscard]] fs::path partition_file(const AdminFixture &f, std::string_view key) {
  return f.db / std::string(kSurfaceDbPartitionDir) /
         (std::string(key) + std::string(kSurfaceDbPartitionExt));
}

// ── describe_db ─────────────────────────────────────────────────────────────

// The description's counts must equal what the build reported and what the
// manifest holds: 3 partitions x 3 symbols, 9 surfaces, non-zero bytes, and
// every partition file present on disk.
TEST(SurfaceDbAdmin, DescribeDbCountsMatchBuild) {
  const AdminFixture f = make_fixture("describe_db");
  const SurfaceDbBuildReport built = build_healthy_db(f);
  const SurfaceDb db = open_db(f);

  const auto desc = describe_db(db);
  ASSERT_TRUE(desc.has_value()) << (desc ? "" : desc.error().to_string());
  EXPECT_EQ(desc->root, f.db.string());
  EXPECT_EQ(desc->n_symbols, std::size_t{3});
  EXPECT_EQ(desc->n_symbols_enabled, std::size_t{3});
  EXPECT_EQ(desc->n_symbols, static_cast<std::size_t>(built.config.n_symbols));
  EXPECT_EQ(desc->n_partitions, std::size_t{3});
  EXPECT_EQ(desc->n_partitions_missing, std::size_t{0});
  EXPECT_EQ(desc->total_surface_count, std::uint64_t{9}); // 3 dates x 3 symbols
  EXPECT_EQ(desc->generation, db.generation());
  EXPECT_GT(desc->total_bytes_on_disk, std::uint64_t{0});
  EXPECT_EQ(desc->total_manifest_bytes, desc->total_bytes_on_disk); // nothing rewritten

  ASSERT_EQ(desc->partitions.size(), std::size_t{3});
  EXPECT_EQ(desc->partitions.front().key, "2026-07-01"); // manifest order = sorted
  std::uint64_t summed = 0;
  for (const DbPartitionSummary &p : desc->partitions) {
    EXPECT_TRUE(p.present) << p.key;
    EXPECT_EQ(p.surface_count, 3u) << p.key;
    EXPECT_GT(p.bytes_on_disk, std::uint64_t{0}) << p.key;
    EXPECT_EQ(p.manifest_bytes, p.bytes_on_disk) << p.key;
    summed += p.surface_count;
  }
  EXPECT_EQ(summed, desc->total_surface_count);
}

// A partition file unlinked behind the manifest's back is reported as absent
// with zero live bytes — the manifest still counts it, `present` says otherwise.
TEST(SurfaceDbAdmin, DescribeDbFlagsMissingPartitionFile) {
  const AdminFixture f = make_fixture("describe_db_missing");
  build_healthy_db(f);
  std::error_code ec;
  ASSERT_TRUE(fs::remove(partition_file(f, "2026-07-02"), ec)) << ec.message();

  const SurfaceDb db = open_db(f);
  const auto desc = describe_db(db);
  ASSERT_TRUE(desc.has_value()) << (desc ? "" : desc.error().to_string());
  EXPECT_EQ(desc->n_partitions, std::size_t{3}); // manifest is unchanged
  EXPECT_EQ(desc->n_partitions_missing, std::size_t{1});

  const auto it = std::find_if(desc->partitions.begin(), desc->partitions.end(),
                               [](const DbPartitionSummary &p) { return p.key == "2026-07-02"; });
  ASSERT_NE(it, desc->partitions.end());
  EXPECT_FALSE(it->present);
  EXPECT_EQ(it->bytes_on_disk, std::uint64_t{0});
  EXPECT_GT(it->manifest_bytes, std::uint64_t{0}); // what it WAS when written
}

// ── describe_partition ──────────────────────────────────────────────────────

// The partition's archive directory carries exactly the fixture's three symbols,
// each with a uid, slices and a non-zero record extent.
TEST(SurfaceDbAdmin, DescribePartitionListsItsSymbols) {
  const AdminFixture f = make_fixture("describe_part");
  build_healthy_db(f);
  const SurfaceDb db = open_db(f);

  const auto part = describe_partition(db, "2026-07-01");
  ASSERT_TRUE(part.has_value()) << (part ? "" : part.error().to_string());
  EXPECT_EQ(part->key, "2026-07-01");
  EXPECT_EQ(part->manifest_surface_count, 3u);
  EXPECT_EQ(part->archive_surface_count, 3u);
  EXPECT_GT(part->bytes_on_disk, std::uint64_t{0});

  std::vector<std::string> names;
  for (const PartitionSymbolInfo &s : part->symbols) {
    names.push_back(s.symbol);
    EXPECT_GT(s.n_slices, 0u) << s.symbol;
    EXPECT_GT(s.surface_bytes, std::uint64_t{0}) << s.symbol;
  }
  EXPECT_EQ(names, (std::vector<std::string>{"AAA", "BBB", "CCC"}));
}

// A key the manifest does not index is NotFound (not an IoError about a path).
TEST(SurfaceDbAdmin, DescribePartitionUnknownKeyIsNotFound) {
  const AdminFixture f = make_fixture("describe_part_bad");
  build_healthy_db(f);
  const SurfaceDb db = open_db(f);

  const auto part = describe_partition(db, "2026-07-04");
  ASSERT_FALSE(part.has_value());
  EXPECT_EQ(part.error().code(), ErrorCode::NotFound);
}

// ── describe_symbol ─────────────────────────────────────────────────────────

// An auto-configured symbol reports enabled, its stored preset, and the curve
// family the fit-policy pinned.
TEST(SurfaceDbAdmin, DescribeSymbolReflectsStoredConfig) {
  const AdminFixture f = make_fixture("describe_sym");
  build_healthy_db(f);
  const SurfaceDb db = open_db(f);

  const auto sym = describe_symbol(db, "AAA");
  ASSERT_TRUE(sym.has_value()) << (sym ? "" : sym.error().to_string());
  EXPECT_EQ(sym->symbol, "AAA");
  EXPECT_TRUE(sym->enabled);
  EXPECT_EQ(sym->preset, FitPreset::Populate);
  EXPECT_TRUE(sym->pin_curve); // generate_symbol_configs pins the policy's family

  const auto stored = db.symbol_config("AAA");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(sym->curve_kind, stored->curve.kind);
  EXPECT_DOUBLE_EQ(sym->band_k, stored->band_k);
  EXPECT_EQ(sym->surface_policy.quality_mode, stored->surface_policy.quality_mode);

  // Case-insensitive, like every other SurfaceDb symbol lookup.
  const auto lower = describe_symbol(db, "aaa");
  ASSERT_TRUE(lower.has_value()) << (lower ? "" : lower.error().to_string());
  EXPECT_EQ(lower->symbol, "AAA");
}

// A symbol stored DISABLED through the real fail-closed path (a board gutted to
// one quote cannot support curve selection) reports enabled == false.
TEST(SurfaceDbAdmin, DescribeSymbolReportsDisabled) {
  const AdminFixture f = make_fixture("describe_sym_disabled");
  {
    auto created = SurfaceDb::create(f.db.string());
    ASSERT_TRUE(created.has_value()) << (created ? "" : created.error().to_string());
    std::vector<CorpusBoard> gutted = load_fixture_boards(f);
    ASSERT_EQ(gutted.size(), std::size_t{9});
    for (CorpusBoard &b : gutted) {
      if (b.symbol == "CCC") {
        ASSERT_FALSE(b.frame.rows.empty());
        b.frame.rows.resize(1); // non-selectable: a single strike cannot pin a curve
      }
    }
    const auto cfg = generate_symbol_configs(*created, gutted, AutoConfigSpec{});
    ASSERT_TRUE(cfg.has_value()) << (cfg ? "" : cfg.error().to_string());
    ASSERT_EQ(cfg->n_disabled_failed, 1u);
  }
  const auto built = build_surface_db(build_spec(f)); // skip-existing keeps CCC disabled
  ASSERT_TRUE(built.has_value()) << (built ? "" : built.error().to_string());
  const SurfaceDb db = open_db(f);

  const auto ccc = describe_symbol(db, "CCC");
  ASSERT_TRUE(ccc.has_value()) << (ccc ? "" : ccc.error().to_string());
  EXPECT_FALSE(ccc->enabled);

  const auto aaa = describe_symbol(db, "AAA");
  ASSERT_TRUE(aaa.has_value());
  EXPECT_TRUE(aaa->enabled);

  // The whole-db view agrees: 3 configured, 2 of them enabled.
  const auto desc = describe_db(db);
  ASSERT_TRUE(desc.has_value()) << (desc ? "" : desc.error().to_string());
  EXPECT_EQ(desc->n_symbols, std::size_t{3});
  EXPECT_EQ(desc->n_symbols_enabled, std::size_t{2});
}

// An unconfigured symbol is NotFound.
TEST(SurfaceDbAdmin, DescribeSymbolUnknownIsNotFound) {
  const AdminFixture f = make_fixture("describe_sym_bad");
  build_healthy_db(f);
  const SurfaceDb db = open_db(f);

  const auto sym = describe_symbol(db, "ZZZ");
  ASSERT_FALSE(sym.has_value());
  EXPECT_EQ(sym.error().code(), ErrorCode::NotFound);
}

// ── query_surface ───────────────────────────────────────────────────────────

// A real cell evaluates to a finite, positive iv and a consistent total
// variance, and reports the surface's uid / slice count.
TEST(SurfaceDbAdmin, QuerySurfaceReturnsFiniteIv) {
  const AdminFixture f = make_fixture("query");
  build_healthy_db(f);
  const SurfaceDb db = open_db(f);

  // Anchor the query at the surface's own forward (an ATM point), exactly what
  // the retired Python map_surface check did.
  const auto mapped = db.map_surface("2026-07-01", "AAA");
  ASSERT_TRUE(mapped.has_value()) << (mapped ? "" : mapped.error().to_string());
  constexpr double kT = 30.0 / 365.0;
  const double fwd = mapped->view.forward_at(kT);
  ASSERT_TRUE(std::isfinite(fwd));

  const auto q = query_surface(db, "2026-07-01", "AAA", fwd, kT);
  ASSERT_TRUE(q.has_value()) << (q ? "" : q.error().to_string());
  EXPECT_EQ(q->key, "2026-07-01");
  EXPECT_EQ(q->symbol, "AAA");
  EXPECT_DOUBLE_EQ(q->K, fwd);
  EXPECT_DOUBLE_EQ(q->T, kT);
  EXPECT_TRUE(std::isfinite(q->iv));
  EXPECT_GT(q->iv, 0.0);
  EXPECT_LT(q->iv, 5.0);
  EXPECT_TRUE(std::isfinite(q->total_variance));
  EXPECT_GT(q->total_variance, 0.0);
  EXPECT_NEAR(q->total_variance, q->iv * q->iv * kT, 1e-12);
  EXPECT_DOUBLE_EQ(q->forward, fwd);
  EXPECT_GT(q->n_slices, std::size_t{0});

  // Same numbers as the zero-copy path the production reader uses.
  EXPECT_DOUBLE_EQ(q->iv, mapped->view.iv(fwd, kT));
  EXPECT_EQ(q->uid, mapped->view.uid());
  EXPECT_EQ(q->n_slices, mapped->view.n_slices());
}

// A key with no partition, and a symbol absent from a real partition, are both
// clean NotFound errors — not a crash and not a zero-filled quote.
TEST(SurfaceDbAdmin, QuerySurfaceBadCellIsCleanError) {
  const AdminFixture f = make_fixture("query_bad");
  build_healthy_db(f);
  const SurfaceDb db = open_db(f);
  constexpr double kT = 30.0 / 365.0;

  const auto no_key = query_surface(db, "2026-07-04", "AAA", 100.0, kT);
  ASSERT_FALSE(no_key.has_value());
  EXPECT_EQ(no_key.error().code(), ErrorCode::NotFound);

  const auto no_sym = query_surface(db, "2026-07-01", "ZZZ", 100.0, kT);
  ASSERT_FALSE(no_sym.has_value());
  EXPECT_EQ(no_sym.error().code(), ErrorCode::NotFound);

  // A non-positive tenor / strike is rejected at the boundary, not evaluated.
  const auto bad_t = query_surface(db, "2026-07-01", "AAA", 100.0, 0.0);
  ASSERT_FALSE(bad_t.has_value());
  EXPECT_EQ(bad_t.error().code(), ErrorCode::InvalidArgument);

  const auto bad_k = query_surface(db, "2026-07-01", "AAA", -1.0, kT);
  ASSERT_FALSE(bad_k.has_value());
  EXPECT_EQ(bad_k.error().code(), ErrorCode::InvalidArgument);
}

// ── verify_db ───────────────────────────────────────────────────────────────

// A healthy database verifies clean: all nine cells map AND evaluate.
TEST(SurfaceDbAdmin, VerifyDbHealthyIsAllOk) {
  const AdminFixture f = make_fixture("verify_ok");
  build_healthy_db(f);
  const SurfaceDb db = open_db(f);

  const auto rep = verify_db(db, DbVerifySpec{});
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_TRUE(rep->ok());
  EXPECT_EQ(rep->n_partitions, std::size_t{3});
  EXPECT_EQ(rep->n_symbols, std::size_t{3});
  EXPECT_EQ(rep->cells_checked, std::size_t{9});
  EXPECT_EQ(rep->cells_ok, std::size_t{9});
  EXPECT_EQ(rep->cells_unmappable, std::size_t{0});
  EXPECT_EQ(rep->cells_non_finite, std::size_t{0});
  EXPECT_TRUE(rep->failures.empty());
  EXPECT_EQ(rep->n_failures_elided, std::size_t{0});
}

// The spec restricts the walk: a key range and a symbol subset shrink the grid
// without changing the verdict.
TEST(SurfaceDbAdmin, VerifyDbHonorsRangeAndSymbolSubset) {
  const AdminFixture f = make_fixture("verify_subset");
  build_healthy_db(f);
  const SurfaceDb db = open_db(f);

  DbVerifySpec spec;
  spec.key_lo = "2026-07-02";
  spec.key_hi = "2026-07-06";
  spec.symbols = {"AAA", "BBB"};
  const auto rep = verify_db(db, spec);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_TRUE(rep->ok());
  EXPECT_EQ(rep->n_partitions, std::size_t{2}); // 07-02, 07-06
  EXPECT_EQ(rep->n_symbols, std::size_t{2});
  EXPECT_EQ(rep->cells_checked, std::size_t{4});
  EXPECT_EQ(rep->cells_ok, std::size_t{4});
}

// A DISABLED symbol is never populated into any partition, so it must be
// excluded by default — otherwise every healthy database reports a missing cell
// on every date. `include_disabled` forces it in, and then the whole column
// shows up as unmappable, each cell identified.
TEST(SurfaceDbAdmin, VerifyDbSkipsDisabledSymbolByDefault) {
  const AdminFixture f = make_fixture("verify_disabled");
  {
    auto created = SurfaceDb::create(f.db.string());
    ASSERT_TRUE(created.has_value()) << (created ? "" : created.error().to_string());
    std::vector<CorpusBoard> gutted = load_fixture_boards(f);
    ASSERT_EQ(gutted.size(), std::size_t{9});
    for (CorpusBoard &b : gutted) {
      if (b.symbol == "CCC") {
        ASSERT_FALSE(b.frame.rows.empty());
        b.frame.rows.resize(1);
      }
    }
    const auto cfg = generate_symbol_configs(*created, gutted, AutoConfigSpec{});
    ASSERT_TRUE(cfg.has_value()) << (cfg ? "" : cfg.error().to_string());
    ASSERT_EQ(cfg->n_disabled_failed, 1u);
  }
  const auto built = build_surface_db(build_spec(f));
  ASSERT_TRUE(built.has_value()) << (built ? "" : built.error().to_string());
  const SurfaceDb db = open_db(f);

  const auto def = verify_db(db, DbVerifySpec{});
  ASSERT_TRUE(def.has_value()) << (def ? "" : def.error().to_string());
  EXPECT_TRUE(def->ok());
  EXPECT_EQ(def->n_symbols, std::size_t{2});     // CCC excluded
  EXPECT_EQ(def->cells_checked, std::size_t{6}); // 3 dates x {AAA, BBB}

  DbVerifySpec forced;
  forced.include_disabled = true;
  const auto all = verify_db(db, forced);
  ASSERT_TRUE(all.has_value()) << (all ? "" : all.error().to_string());
  EXPECT_FALSE(all->ok());
  EXPECT_EQ(all->n_symbols, std::size_t{3});
  EXPECT_EQ(all->cells_checked, std::size_t{9});
  EXPECT_EQ(all->cells_ok, std::size_t{6});
  EXPECT_EQ(all->cells_unmappable, std::size_t{3}); // CCC on all three dates
  ASSERT_EQ(all->failures.size(), std::size_t{3});
  for (const DbCellFault &fault : all->failures) {
    EXPECT_EQ(fault.symbol, "CCC");
    EXPECT_EQ(fault.kind, DbCellFailure::Unmappable);
    EXPECT_FALSE(fault.detail.empty());
  }
}

// A partition file unlinked from under the manifest breaks every cell in that
// date. Verify must report the failure with the cell identified — key AND symbol
// — and keep the rest of the database's verdict intact.
TEST(SurfaceDbAdmin, VerifyDbReportsBrokenPartitionCells) {
  const AdminFixture f = make_fixture("verify_broken");
  build_healthy_db(f);
  std::error_code ec;
  ASSERT_TRUE(fs::remove(partition_file(f, "2026-07-02"), ec)) << ec.message();
  const SurfaceDb db = open_db(f);

  const auto rep = verify_db(db, DbVerifySpec{});
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_FALSE(rep->ok());
  EXPECT_EQ(rep->cells_checked, std::size_t{9});
  EXPECT_EQ(rep->cells_ok, std::size_t{6});
  EXPECT_EQ(rep->cells_unmappable, std::size_t{3});
  EXPECT_EQ(rep->cells_non_finite, std::size_t{0});
  EXPECT_EQ(rep->n_failures_elided, std::size_t{0}); // 3 < the default cap

  ASSERT_EQ(rep->failures.size(), std::size_t{3});
  std::vector<std::string> broken;
  for (const DbCellFault &fault : rep->failures) {
    EXPECT_EQ(fault.key, "2026-07-02"); // the damaged date, identified
    EXPECT_EQ(fault.kind, DbCellFailure::Unmappable);
    EXPECT_FALSE(fault.detail.empty()); // carries the mapping error text
    broken.push_back(fault.symbol);
  }
  std::sort(broken.begin(), broken.end());
  EXPECT_EQ(broken, (std::vector<std::string>{"AAA", "BBB", "CCC"}));
}

// The failure list is CAPPED, and the cap is not silent: what does not fit is
// counted in n_failures_elided while the totals stay exact. A truncated list
// that reported nothing elided would read as "everything else passed".
TEST(SurfaceDbAdmin, VerifyDbCapsFailureListAndCountsElisions) {
  const AdminFixture f = make_fixture("verify_cap");
  build_healthy_db(f);
  std::error_code ec;
  ASSERT_TRUE(fs::remove(partition_file(f, "2026-07-02"), ec)) << ec.message();
  const SurfaceDb db = open_db(f);

  DbVerifySpec spec;
  spec.max_reported_failures = 2;
  const auto rep = verify_db(db, spec);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_FALSE(rep->ok());
  EXPECT_EQ(rep->cells_unmappable, std::size_t{3}); // totals are never truncated
  EXPECT_EQ(rep->failures.size(), std::size_t{2});  // capped
  EXPECT_EQ(rep->n_failures_elided, std::size_t{1});

  // A zero cap retains no detail at all, and says so.
  DbVerifySpec none;
  none.max_reported_failures = 0;
  const auto quiet = verify_db(db, none);
  ASSERT_TRUE(quiet.has_value()) << (quiet ? "" : quiet.error().to_string());
  EXPECT_TRUE(quiet->failures.empty());
  EXPECT_EQ(quiet->n_failures_elided, std::size_t{3});
  EXPECT_EQ(quiet->cells_unmappable, std::size_t{3});
}

// A spec that cannot be honored is an Err, not a silently-degraded walk.
TEST(SurfaceDbAdmin, VerifyDbRejectsBadProbeTenor) {
  const AdminFixture f = make_fixture("verify_badspec");
  build_healthy_db(f);
  const SurfaceDb db = open_db(f);

  DbVerifySpec spec;
  spec.probe_T = 0.0;
  const auto zero = verify_db(db, spec);
  ASSERT_FALSE(zero.has_value());
  EXPECT_EQ(zero.error().code(), ErrorCode::InvalidArgument);

  spec.probe_T = -1.0;
  const auto negative = verify_db(db, spec);
  ASSERT_FALSE(negative.has_value());
  EXPECT_EQ(negative.error().code(), ErrorCode::InvalidArgument);

  DbVerifySpec empty_sym;
  empty_sym.symbols = {""};
  const auto bad_sym = verify_db(db, empty_sym);
  ASSERT_FALSE(bad_sym.has_value());
  EXPECT_EQ(bad_sym.error().code(), ErrorCode::InvalidArgument);
}

// An empty database (no partitions) verifies vacuously ok — zero cells checked.
// A verifier that failed on "nothing to check" would block a fresh root.
TEST(SurfaceDbAdmin, VerifyDbEmptyDatabaseIsVacuouslyOk) {
  const fs::path root = fresh_dir("verify_empty");
  auto created = SurfaceDb::create((root / "db").string());
  ASSERT_TRUE(created.has_value()) << (created ? "" : created.error().to_string());

  const auto rep = verify_db(*created, DbVerifySpec{});
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_TRUE(rep->ok());
  EXPECT_EQ(rep->n_partitions, std::size_t{0});
  EXPECT_EQ(rep->cells_checked, std::size_t{0});

  const auto desc = describe_db(*created);
  ASSERT_TRUE(desc.has_value()) << (desc ? "" : desc.error().to_string());
  EXPECT_EQ(desc->n_partitions, std::size_t{0});
  EXPECT_EQ(desc->n_symbols, std::size_t{0});
  EXPECT_EQ(desc->total_bytes_on_disk, std::uint64_t{0});
}

} // namespace
} // namespace atx::vol
