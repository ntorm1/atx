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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/corpus.hpp"                // CorpusBoard
#include "atx/vol/detail/archive_util.hpp"   // crc32c (test-side record CRC repair)
#include "atx/vol/opra_batch.hpp"            // corpus_board_from_opra
#include "atx/vol/opra_hive.hpp"             // OpraHiveSpec, load_opra_hive
#include "atx/vol/session.hpp"               // FitPreset
#include "atx/vol/surface_archive.hpp"       // ArchiveV2SurfaceHeader, ArchiveV2DirEntry
#include "atx/vol/surface_db.hpp"            // SurfaceDb
#include "atx/vol/surface_db_admin.hpp"      // the unit under test
#include "atx/vol/surface_db_build.hpp"      // build_surface_db, generate_symbol_configs
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

// ── byte-level record surgery (real damage to a real database) ──────────────
//
// Whole-file TRUNCATION is already caught at open by the header/metadata CRCs, so
// it proves nothing about the payload. These helpers damage ONE record IN PLACE,
// preserving the file's length and the archive's framing — the bit-rot / partial-
// copy / hand-edit case that maps cleanly and is only visible to the record's own
// payload checksum.

// The on-disk extent of one surface record, read from the archive's OWN directory
// so the test never hard-codes a layout. The db + mapping are released before this
// returns (Windows will not let a mapped file be rewritten).
struct RecordExtent {
  std::uint64_t offset{0};
  std::uint64_t size{0};
};
[[nodiscard]] RecordExtent record_extent(const AdminFixture &f, std::string_view key,
                                         std::string_view symbol) {
  RecordExtent out;
  const SurfaceDb db = open_db(f);
  const auto archive = db.open_partition(key);
  EXPECT_TRUE(archive.has_value()) << (archive ? "" : archive.error().to_string());
  if (!archive) {
    return out;
  }
  for (const ArchiveV2DirEntry &e : archive->directory()) {
    const std::string name(e.symbol, std::min<std::size_t>(e.symbol_len, sizeof(e.symbol)));
    if (name == symbol) {
      out.offset = e.surface_offset;
      out.size = e.surface_size;
      break;
    }
  }
  EXPECT_GT(out.size, std::uint64_t{0}) << symbol;
  return out;
}

// Recompute a record's payload CRC over its (already-edited) bytes — the exact
// rule `record_crc_v2` applies: CRC-32C of the whole record with the checksum
// field itself zeroed. Used to make a REWRITTEN record self-consistent, so a case
// can exercise a non-checksum failure without tripping the checksum one.
void repair_record_crc(std::span<std::byte> record) {
  constexpr std::size_t kCrcOff = offsetof(ArchiveV2SurfaceHeader, payload_crc32c);
  const std::uint32_t zero = 0;
  std::memcpy(record.data() + kCrcOff, &zero, sizeof zero);
  const std::uint32_t crc = detail::crc32c(record.data(), record.size());
  std::memcpy(record.data() + kCrcOff, &crc, sizeof crc);
}

// Apply `mutate` to one record's bytes and write the partition file back. The span
// is the record's exact extent, so an edit can never change the file's length.
template <class Fn>
void edit_record(const AdminFixture &f, std::string_view key, std::string_view symbol,
                 Fn &&mutate) {
  const RecordExtent ext = record_extent(f, key, symbol);
  ASSERT_GT(ext.size, std::uint64_t{0});
  const fs::path path = partition_file(f, key);

  std::vector<std::byte> bytes;
  {
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good()) << path.string();
    in.seekg(0, std::ios::end);
    const auto n = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    bytes.resize(n);
    in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(n));
    ASSERT_EQ(static_cast<std::size_t>(in.gcount()), n);
  }
  ASSERT_LE(ext.offset + ext.size, bytes.size());
  mutate(std::span<std::byte>(bytes.data() + ext.offset, static_cast<std::size_t>(ext.size)));

  const std::size_t before = bytes.size();
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << path.string();
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(out.good());
  }
  ASSERT_EQ(fs::file_size(path), before) << "in-place edit must preserve the file length";
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
// family the fit-policy chose (recorded as the preferred route — the auto-config
// stage does not pin unless asked, so the fitter's fallback ladders stay alive).
TEST(SurfaceDbAdmin, DescribeSymbolReflectsStoredConfig) {
  const AdminFixture f = make_fixture("describe_sym");
  build_healthy_db(f);
  const SurfaceDb db = open_db(f);

  const auto sym = describe_symbol(db, "AAA");
  ASSERT_TRUE(sym.has_value()) << (sym ? "" : sym.error().to_string());
  EXPECT_EQ(sym->symbol, "AAA");
  EXPECT_TRUE(sym->enabled);
  EXPECT_EQ(sym->preset, FitPreset::Populate);
  EXPECT_FALSE(sym->pin_curve); // generate_symbol_configs records, does not pin

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
  EXPECT_EQ(rep->cells_checksum, std::size_t{0}); // every payload matches its stored CRC
  EXPECT_EQ(rep->n_partitions_in_db, std::size_t{3});
  EXPECT_FALSE(rep->selected_no_cells());
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
  // FIX-C-2. The verdict stays `ok` — a fail-closed disable is a legitimate state,
  // not a corrupt database — but the walk must SAY which columns it dropped.
  // Without this, an `ok` over a database permanently missing a requested symbol
  // is byte-for-byte an `ok` over a complete one: the symbol is simply not a
  // column, so nothing counts it and nothing names it.
  ASSERT_EQ(def->disabled_symbols.size(), std::size_t{1});
  EXPECT_EQ(def->disabled_symbols.front(), "CCC");

  DbVerifySpec forced;
  forced.include_disabled = true;
  const auto all = verify_db(db, forced);
  ASSERT_TRUE(all.has_value()) << (all ? "" : all.error().to_string());
  EXPECT_FALSE(all->ok());
  EXPECT_EQ(all->n_symbols, std::size_t{3});
  // Nothing was dropped this time, so there is nothing to name.
  EXPECT_TRUE(all->disabled_symbols.empty());
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

// ── zero cells over a POPULATED database ────────────────────────────────────
//
// A walk that selected nothing found nothing broken, so the counters are all zero
// and every failure list is empty. That is not health: three real partitions sat
// on disk and not one byte of them was read. `ok()` must not be true here.

// Every symbol fail-closed DISABLED, with the partitions still on disk. The
// default walk drops every column, so `symbols 0 / cells_checked 0` — and the
// database it never opened is the one the operator is about to trade on.
TEST(SurfaceDbAdmin, VerifyDbAllSymbolsDisabledOverPopulatedDbIsNotOk) {
  const AdminFixture f = make_fixture("verify_all_disabled");
  build_healthy_db(f);
  {
    SurfaceDb db = open_db(f);
    for (const std::string &name : db.symbols()) {
      auto cfg = db.symbol_config(name);
      ASSERT_TRUE(cfg.has_value()) << (cfg ? "" : cfg.error().to_string());
      cfg->enabled = false;
      const Status st = db.upsert_symbol(name, *cfg);
      ASSERT_TRUE(st.has_value()) << (st ? "" : st.error().to_string());
    }
  }
  const SurfaceDb db = open_db(f);

  const auto rep = verify_db(db, DbVerifySpec{});
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->n_partitions, std::size_t{3}); // the partitions are all still there
  EXPECT_EQ(rep->n_partitions_in_db, std::size_t{3});
  EXPECT_EQ(rep->n_symbols, std::size_t{0}); // ...and not one column was selected
  EXPECT_EQ(rep->cells_checked, std::size_t{0});
  EXPECT_TRUE(rep->failures.empty());
  EXPECT_TRUE(rep->selected_no_cells());
  EXPECT_FALSE(rep->ok()) << "a walk that opened nothing over a populated db is not health";
  // And the report says WHICH names it dropped, so `symbols 0` is not a riddle.
  EXPECT_EQ(rep->disabled_symbols,
            (std::vector<std::string>{"AAA", "BBB", "CCC"})); // canonical + sorted

  // ...and it is not a blanket "zero cells is an error": forcing the disabled
  // columns back in checks all nine and passes, so the verdict tracks what was
  // actually covered, not merely whether the symbols are enabled.
  DbVerifySpec forced;
  forced.include_disabled = true;
  const auto all = verify_db(db, forced);
  ASSERT_TRUE(all.has_value()) << (all ? "" : all.error().to_string());
  EXPECT_EQ(all->cells_checked, std::size_t{9});
  EXPECT_FALSE(all->selected_no_cells());
  EXPECT_TRUE(all->ok());
}

// A `--from`/`--to` window that matches no partition. Same shape, different door:
// the database is fine and full, the RUN checked nothing.
TEST(SurfaceDbAdmin, VerifyDbKeyRangeMatchingNothingIsNotOk) {
  const AdminFixture f = make_fixture("verify_empty_range");
  build_healthy_db(f);
  const SurfaceDb db = open_db(f);

  DbVerifySpec spec;
  spec.key_lo = "2026-08-01"; // the fixture is all July
  spec.key_hi = "2026-08-31";
  const auto rep = verify_db(db, spec);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->n_partitions, std::size_t{0});       // no rows in range...
  EXPECT_EQ(rep->n_partitions_in_db, std::size_t{3}); // ...over a database that is full
  EXPECT_EQ(rep->cells_checked, std::size_t{0});
  EXPECT_TRUE(rep->selected_no_cells());
  EXPECT_FALSE(rep->ok()) << "a range that selected no cells over a populated db is not health";

  // The range that DOES match is unaffected — this is not "ranges are suspect".
  DbVerifySpec july;
  july.key_lo = "2026-07-01";
  july.key_hi = "2026-07-31";
  const auto hit = verify_db(db, july);
  ASSERT_TRUE(hit.has_value()) << (hit ? "" : hit.error().to_string());
  EXPECT_EQ(hit->cells_checked, std::size_t{9});
  EXPECT_TRUE(hit->ok());
}

// ── payload checksum ────────────────────────────────────────────────────────

// One byte flipped INSIDE a record, length preserved. The archive's framing,
// magic and bounds are all still correct, so the surface maps and the ATM probe
// returns a perfectly usable number — the walk sees nothing wrong. The record's
// own payload CRC, which the format has carried all along, says it is corrupt.
TEST(SurfaceDbAdmin, VerifyDbDetectsInPlacePayloadCorruption) {
  const AdminFixture f = make_fixture("verify_crc");
  build_healthy_db(f);
  // Flip the LAST byte of AAA's record on 2026-07-01: deep in the payload, past
  // everything `map_symbol` / `create_over_record` look at.
  edit_record(f, "2026-07-01", "AAA", [](std::span<std::byte> rec) {
    rec.back() ^= std::byte{0xFF};
  });
  const SurfaceDb db = open_db(f);

  // The damage is INVISIBLE to the map + probe walk...
  const auto mapped = db.map_surface("2026-07-01", "AAA");
  ASSERT_TRUE(mapped.has_value()) << (mapped ? "" : mapped.error().to_string());
  const double fwd = mapped->view.forward_at(kSurfaceDbVerifyProbeT);
  const double iv = mapped->view.iv(fwd, kSurfaceDbVerifyProbeT);
  EXPECT_TRUE(std::isfinite(fwd));
  EXPECT_GT(fwd, 0.0);
  EXPECT_TRUE(std::isfinite(iv));
  EXPECT_GT(iv, 0.0);

  // ...and plainly visible to the checksum the record already carries.
  const auto archive = db.open_partition("2026-07-01");
  ASSERT_TRUE(archive.has_value()) << (archive ? "" : archive.error().to_string());
  const Status crc = archive->validate_symbol("AAA");
  EXPECT_FALSE(crc.has_value()) << "the fixture must actually corrupt the payload";

  const auto rep = verify_db(db, DbVerifySpec{});
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->cells_checked, std::size_t{9});
  EXPECT_EQ(rep->cells_ok, std::size_t{8});
  EXPECT_FALSE(rep->ok()) << "verify must not pass a database whose bytes fail their own CRC";
  // Its OWN kind: not lumped in with an unmappable file or a bad number, in the
  // counters or in the failure list.
  EXPECT_EQ(rep->cells_checksum, std::size_t{1});
  EXPECT_EQ(rep->cells_unmappable, std::size_t{0});
  EXPECT_EQ(rep->cells_non_finite, std::size_t{0});
  ASSERT_EQ(rep->failures.size(), std::size_t{1});
  EXPECT_EQ(rep->failures.front().key, "2026-07-01"); // attributed to the exact cell
  EXPECT_EQ(rep->failures.front().symbol, "AAA");
  EXPECT_EQ(rep->failures.front().kind, DbCellFailure::ChecksumMismatch);
  EXPECT_FALSE(rep->failures.front().detail.empty()); // carries the checksum error text

  // The counters still exhaust the walk.
  EXPECT_EQ(rep->cells_ok + rep->cells_unmappable + rep->cells_non_finite + rep->cells_checksum,
            rep->cells_checked);
}

// ── the ATM probe branch ────────────────────────────────────────────────────

// A stored surface whose forward curve is degenerate (spot AND every slice forward
// rewritten to 0) but whose record CRC is REPAIRED, so the bytes are perfectly
// self-consistent. Mapping proves nothing about it; only the ATM evaluation can
// tell it apart from a healthy cell. This is the one thing that makes `verify`
// more than a map-only walk, and it must be reported as its own failure kind, not
// as a checksum fault.
TEST(SurfaceDbAdmin, VerifyDbFlagsNonFiniteAtmProbe) {
  const AdminFixture f = make_fixture("verify_nonfinite");
  build_healthy_db(f);
  edit_record(f, "2026-07-06", "BBB", [](std::span<std::byte> rec) {
    ArchiveV2SurfaceHeader h{};
    std::memcpy(&h, rec.data(), sizeof h);
    const double zero = 0.0;
    std::memcpy(rec.data() + offsetof(ArchiveV2SurfaceHeader, S), &zero, sizeof zero);
    for (std::uint32_t i = 0; i < h.n_slices; ++i) {
      std::memcpy(rec.data() + h.col_forward_off + i * sizeof(double), &zero, sizeof zero);
    }
    repair_record_crc(rec);
  });
  const SurfaceDb db = open_db(f);

  // The record is BYTE-VALID — the checksum branch must stay silent about it.
  const auto archive = db.open_partition("2026-07-06");
  ASSERT_TRUE(archive.has_value()) << (archive ? "" : archive.error().to_string());
  const Status crc = archive->validate_symbol("BBB");
  EXPECT_TRUE(crc.has_value()) << (crc ? "" : crc.error().to_string());

  // It still maps — the failure is only reachable by EVALUATING it.
  const auto mapped = db.map_surface("2026-07-06", "BBB");
  ASSERT_TRUE(mapped.has_value()) << (mapped ? "" : mapped.error().to_string());
  const double fwd = mapped->view.forward_at(kSurfaceDbVerifyProbeT);
  EXPECT_FALSE(std::isfinite(fwd) && fwd > 0.0) << "fwd=" << fwd;

  const auto rep = verify_db(db, DbVerifySpec{});
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_FALSE(rep->ok());
  EXPECT_EQ(rep->cells_checked, std::size_t{9});
  EXPECT_EQ(rep->cells_ok, std::size_t{8});
  EXPECT_EQ(rep->cells_unmappable, std::size_t{0});
  EXPECT_EQ(rep->cells_non_finite, std::size_t{1});
  EXPECT_EQ(rep->cells_checksum, std::size_t{0}); // the bytes are fine; the SURFACE is not
  ASSERT_EQ(rep->failures.size(), std::size_t{1});
  EXPECT_EQ(rep->failures.front().key, "2026-07-06");
  EXPECT_EQ(rep->failures.front().symbol, "BBB");
  EXPECT_EQ(rep->failures.front().kind, DbCellFailure::NonFinite);
  EXPECT_FALSE(rep->failures.front().detail.empty()); // carries the offending numbers

  // The spot check agrees with the walk: `query` must not print a number for a
  // cell `verify` calls broken.
  const auto q = query_surface(db, "2026-07-06", "BBB", 100.0, kSurfaceDbVerifyProbeT);
  ASSERT_FALSE(q.has_value()) << "query_surface must reject what verify_db rejects";
  EXPECT_EQ(q.error().code(), ErrorCode::Internal);

  // Every other cell is untouched.
  const auto aaa = query_surface(db, "2026-07-06", "AAA",
                                 db.map_surface("2026-07-06", "AAA")->view.forward_at(
                                     kSurfaceDbVerifyProbeT),
                                 kSurfaceDbVerifyProbeT);
  EXPECT_TRUE(aaa.has_value()) << (aaa ? "" : aaa.error().to_string());
}

// A genuinely FRESH database (no partitions at all) verifies vacuously ok — the
// one zero-cell shape that stays green. A verifier that failed on "there is
// nothing here yet" would block a newly created root, and the operator's real
// question there ("this should not still be empty") is `--min-cells`, which only
// the operator can answer. Contrast the two cases above: partitions exist and the
// walk read none of them.
TEST(SurfaceDbAdmin, VerifyDbEmptyDatabaseIsVacuouslyOk) {
  const fs::path root = fresh_dir("verify_empty");
  auto created = SurfaceDb::create((root / "db").string());
  ASSERT_TRUE(created.has_value()) << (created ? "" : created.error().to_string());

  const auto rep = verify_db(*created, DbVerifySpec{});
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_TRUE(rep->ok());
  EXPECT_EQ(rep->n_partitions, std::size_t{0});
  EXPECT_EQ(rep->n_partitions_in_db, std::size_t{0}); // nothing to have been wrong about
  EXPECT_FALSE(rep->selected_no_cells());
  EXPECT_EQ(rep->cells_checked, std::size_t{0});

  const auto desc = describe_db(*created);
  ASSERT_TRUE(desc.has_value()) << (desc ? "" : desc.error().to_string());
  EXPECT_EQ(desc->n_partitions, std::size_t{0});
  EXPECT_EQ(desc->n_symbols, std::size_t{0});
  EXPECT_EQ(desc->total_bytes_on_disk, std::uint64_t{0});
}

} // namespace
} // namespace atx::vol
