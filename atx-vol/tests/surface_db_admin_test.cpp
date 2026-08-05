// SurfaceDbAdmin suite — proves the CLI-native management/inspection layer
// (atx/vol/tools/surface_db_admin.hpp) answers, from C++ alone, every question the
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
#include <optional>
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
#include "atx/vol/tools/surface_db_admin.hpp"      // the unit under test
#include "atx/vol/tools/surface_db_build.hpp"      // build_surface_db, generate_symbol_configs
#include "atx/vol/types.hpp"
#include "atx/vol/vol_curve.hpp"             // VolCurveKind

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

// ── losing ONE cell the way production loses one (FIX-H) ────────────────────
//
// Rewrite `key` with every symbol it holds EXCEPT `drop`, through the real
// `SurfaceDb::write_partition`. This is not a synthetic mutilation: a partition
// rewrite is whole-file, so a cell whose re-fit fails is dropped exactly like
// this, and a cell that never fitted was never in the file to begin with. The two
// are byte-for-byte identical afterwards — which is the whole reason `verify`
// cannot judge an absence and must name it instead.
//
// The manifest is updated with the file (write_partition does both), so the
// result is a consistent database that is simply missing one cell.
void drop_cell(const AdminFixture &f, std::string_view key, std::string_view drop) {
  SurfaceDb db = open_db(f);
  std::vector<std::string> names;
  std::vector<PricedSurface> surfaces;
  {
    const auto part = db.open_partition(key);
    ASSERT_TRUE(part.has_value()) << (part ? "" : part.error().to_string());
    auto all = part->reconstruct_all(); // directory order, same order as directory()
    ASSERT_TRUE(all.has_value()) << (all ? "" : all.error().to_string());
    const std::span<const ArchiveV2DirEntry> dir = part->directory();
    ASSERT_EQ(dir.size(), all->size());
    for (std::size_t i = 0; i < dir.size(); ++i) {
      const std::string name(dir[i].symbol,
                             std::min<std::size_t>(dir[i].symbol_len, sizeof(dir[i].symbol)));
      if (name == drop) {
        continue;
      }
      names.push_back(name);
      surfaces.push_back(std::move((*all)[i]));
    }
  }
  ASSERT_FALSE(names.empty()) << "dropping the only symbol would be a different fixture";
  ASSERT_EQ(names.size(), surfaces.size());
  // Pointers are taken only after `surfaces` has stopped growing.
  std::vector<SurfaceArchiveItem> items;
  items.reserve(names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    items.push_back(SurfaceArchiveItem{names[i], &surfaces[i], std::nullopt});
  }
  const Status wrote = db.write_partition(key, items);
  ASSERT_TRUE(wrote.has_value()) << (wrote ? "" : wrote.error().to_string());
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
  EXPECT_EQ(rep->cells_absent, std::size_t{0}); // a full grid is missing nothing
  EXPECT_EQ(rep->cells_unmappable, std::size_t{0});
  EXPECT_EQ(rep->cells_non_finite, std::size_t{0});
  EXPECT_EQ(rep->cells_checksum, std::size_t{0}); // every payload matches its stored CRC
  EXPECT_EQ(rep->n_partitions_in_db, std::size_t{3});
  EXPECT_FALSE(rep->selected_no_cells());
  EXPECT_TRUE(rep->failures.empty());
  EXPECT_EQ(rep->n_failures_elided, std::size_t{0});
  EXPECT_TRUE(rep->absent_cells.empty());
  EXPECT_EQ(rep->n_absent_elided, std::size_t{0});
  // Gate -1 over a database written by the real writer: every manifest record
  // still describes its file. This is the false-positive guard for the check
  // below — a cross-check that fired on a freshly built database would be worse
  // than no cross-check at all.
  EXPECT_EQ(rep->partitions_index_mismatch, std::size_t{0});
  EXPECT_TRUE(rep->index_faults.empty());
  EXPECT_EQ(rep->n_index_faults_elided, std::size_t{0});
}

// The "opens but is wrong" case: the partition FILE is a perfectly good file and
// the manifest RECORD describes a different one.
//
// `write_partition` writes the archive first and the manifest second — correct,
// because the reverse would let an interrupted archive write advance the index —
// so a crash or a manifest-write error BETWEEN the two leaves exactly this. It is
// reproduced here without a crash, by dropping a narrower database's partition
// file on top of this one's: same key, fewer surfaces, different size, while the
// manifest keeps the record of the write it no longer has.
//
// Every per-cell gate passes. That is the whole point — the bytes are sound, the
// cells map and checksum, and the only thing wrong is the index. Nothing else in
// the tool can see it.
TEST(SurfaceDbAdmin, VerifyDbCatchesAPartitionRecordThatDisagreesWithItsFile) {
  const AdminFixture f = make_fixture("verify_index_mismatch");
  build_healthy_db(f); // 3 symbols x 3 dates

  // A second database over the SAME hive but a NARROWER universe, so its
  // partitions hold 2 surfaces where the first holds 3.
  AdminFixture narrow = f;
  narrow.db = f.root / "db_narrow";
  {
    SurfaceDbBuildSpec nspec = build_spec(narrow);
    nspec.hive.symbols = {f.fx.symbols[0], f.fx.symbols[1]};
    const auto nrep = build_surface_db(nspec);
    ASSERT_TRUE(nrep.has_value()) << (nrep ? "" : nrep.error().to_string());
    ASSERT_EQ(nrep->coverage.cells_ok, 6u);
  }

  const std::string key = f.fx.dates.front();
  const std::uintmax_t narrow_bytes = fs::file_size(partition_file(narrow, key));
  fs::copy_file(partition_file(narrow, key), partition_file(f, key),
                fs::copy_options::overwrite_existing);

  const SurfaceDb db = open_db(f);
  DbVerifySpec spec;
  spec.key_lo = key; // scope to the damaged row so the counters are unambiguous
  spec.key_hi = key;
  const auto rep = verify_db(db, spec);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());

  EXPECT_EQ(rep->partitions_index_mismatch, std::size_t{1});
  ASSERT_EQ(rep->index_faults.size(), std::size_t{1});
  EXPECT_EQ(rep->index_faults[0].key, key);
  EXPECT_EQ(rep->index_faults[0].manifest_surface_count, 3u); // what the record claims
  EXPECT_EQ(rep->index_faults[0].archive_surface_count, 2u);  // what the file holds
  EXPECT_EQ(rep->index_faults[0].bytes_on_disk, static_cast<std::uint64_t>(narrow_bytes));
  EXPECT_EQ(rep->n_index_faults_elided, std::size_t{0});
  // Note which half of the check earned this: at fixture scale BOTH files are
  // block-padded to the same size, so the byte comparison sees nothing and the
  // SURFACE COUNT is what catches it. That is the argument for comparing both —
  // either number can move without the other.

  // It MOVES the verdict — unlike absence, a stale record is never a healthy
  // steady state.
  EXPECT_FALSE(rep->ok());

  // And it is the ONLY thing wrong: no cell is damaged. The two symbols the file
  // still holds pass every gate; the third is simply not there, which is absence,
  // not a fault.
  EXPECT_EQ(rep->cells_checked, std::size_t{3});
  EXPECT_EQ(rep->cells_ok, std::size_t{2});
  EXPECT_EQ(rep->cells_absent, std::size_t{1});
  EXPECT_EQ(rep->cells_unmappable, std::size_t{0});
  EXPECT_EQ(rep->cells_checksum, std::size_t{0});
  EXPECT_EQ(rep->cells_non_finite, std::size_t{0});
  EXPECT_TRUE(rep->failures.empty());
}

// A partition file that will not open at all is NOT an index fault. Every one of
// its cells is already reported `unmappable`, which is louder and more precise, so
// counting the row twice would only double-report the same bytes.
TEST(SurfaceDbAdmin, VerifyDbUnopenablePartitionIsNotCountedAsAnIndexMismatch) {
  const AdminFixture f = make_fixture("verify_index_unopenable");
  build_healthy_db(f);
  const std::string key = f.fx.dates.front();
  ASSERT_TRUE(fs::remove(partition_file(f, key)));

  const SurfaceDb db = open_db(f);
  DbVerifySpec spec;
  spec.key_lo = key;
  spec.key_hi = key;
  const auto rep = verify_db(db, spec);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->partitions_index_mismatch, std::size_t{0});
  EXPECT_TRUE(rep->index_faults.empty());
  EXPECT_EQ(rep->cells_unmappable, std::size_t{3}); // the loud answer, unchanged
  EXPECT_FALSE(rep->ok());
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

// A symbol disabled at CONFIG time — before it ever fitted, which is what this
// fixture builds (CCC is gutted so selection refuses it on the first run) — is
// never populated into any partition, so it must be excluded by default:
// otherwise every healthy database reports a missing cell on every date.
// `include_disabled` forces it in, and then the whole column shows up as ABSENT,
// each cell identified — and the database still verifies CLEAN.
//
// That last clause is FIX-H's, and it is what makes `--include-disabled` worth
// running: before it, forcing the column in reported three `unmappable` faults
// and `verdict FAILED`, so the flag whose job is to PROVE a disabled name is not
// there could only ever answer by calling the database broken. Now it answers the
// question that was asked — these three cells were never stored — without
// claiming anything is wrong.
//
// SCOPE, deliberately narrow (FIX-E): this is NOT the general invariant "a
// disabled symbol is never in any partition". A symbol disabled AFTER it fitted
// keeps its stored surfaces — `enabled = false` means stop fitting, not delete —
// so for that symbol the default skip leaves REAL cells unwalked and
// `--include-disabled` reports what is there rather than proving absence. This
// test exercises only the config-time case, which is why it is still correct.
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
  EXPECT_TRUE(all->ok()) << "never-stored cells are not a corrupt database";
  EXPECT_EQ(all->n_symbols, std::size_t{3});
  // Nothing was dropped this time, so there is nothing to name.
  EXPECT_TRUE(all->disabled_symbols.empty());
  EXPECT_EQ(all->cells_checked, std::size_t{9});
  EXPECT_EQ(all->cells_ok, std::size_t{6});
  EXPECT_EQ(all->cells_absent, std::size_t{3}); // CCC on all three dates
  EXPECT_EQ(all->cells_unmappable, std::size_t{0});
  // Absent, so nothing is on the FAULT list at all — the answer is on its own.
  EXPECT_TRUE(all->failures.empty());
  EXPECT_EQ(all->n_failures_elided, std::size_t{0});
  ASSERT_EQ(all->absent_cells.size(), std::size_t{3});
  EXPECT_EQ(all->n_absent_elided, std::size_t{0});
  for (const DbAbsentCell &cell : all->absent_cells) {
    EXPECT_EQ(cell.symbol, "CCC");
    EXPECT_FALSE(cell.key.empty()); // the DATE is the identity that lets a set be diffed
  }
}

// A partition file unlinked from under the manifest breaks every cell in that
// date. Verify must report the failure with the cell identified — key AND symbol
// — and keep the rest of the database's verdict intact.
//
// THE FIX-H BOUNDARY, and the reason this test is now load-bearing twice over: a
// vanished partition file must NOT be excused as absence. The directory that
// decides "was this ever stored?" lives INSIDE the file, so when the file is gone
// the question is unanswerable — and the honest answer to an unanswerable
// question about stored data is the corruption one. If this ever starts counting
// `cells_absent`, the single loudest form of data loss the tool can see (a whole
// date deleted) has gone quiet and green.
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
  EXPECT_EQ(rep->cells_absent, std::size_t{0}) << "an unreadable partition is not an absent cell";
  EXPECT_TRUE(rep->absent_cells.empty());
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

// ── absence vs corruption (FIX-H) ───────────────────────────────────────────
//
// The defect these three pin: `verify_db` reported a cell that was NEVER STORED
// and a cell that was stored and can no longer be read as the same thing, so the
// finished production database — 9 permanently-unfittable cells out of 867,
// re-fitting nothing, entirely healthy — printed `verdict FAILED` and exited 1 on
// every run. A permanently-red verdict is not a signal, and the signal it was
// drowning is the one that would catch a real, measured, still-unfixed data-loss
// path: a whole-partition rewrite destroying stored surfaces.

// A cell the database does not hold, in the shape production actually has it: an
// ENABLED symbol that fits on two dates and is missing on the third. The verdict
// must be clean, and the cell must still be counted and NAMED — the whole design
// is "loud but not a failure", and dropping either half of that is the failure
// mode on the other side.
TEST(SurfaceDbAdmin, VerifyDbNeverStoredCellIsAbsentNotAFailure) {
  const AdminFixture f = make_fixture("verify_absent");
  build_healthy_db(f);
  drop_cell(f, "2026-07-02", "CCC");
  const SurfaceDb db = open_db(f);

  const auto rep = verify_db(db, DbVerifySpec{});
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_TRUE(rep->ok()) << "a converged database missing a permanently-unfittable cell is healthy";
  EXPECT_EQ(rep->cells_checked, std::size_t{9});
  EXPECT_EQ(rep->cells_ok, std::size_t{8});
  EXPECT_EQ(rep->cells_absent, std::size_t{1});
  // NOT a fault, in any of the three counters or on the fault list.
  EXPECT_EQ(rep->cells_unmappable, std::size_t{0});
  EXPECT_EQ(rep->cells_non_finite, std::size_t{0});
  EXPECT_EQ(rep->cells_checksum, std::size_t{0});
  EXPECT_TRUE(rep->failures.empty());
  EXPECT_EQ(rep->n_failures_elided, std::size_t{0});

  // ...and still fully identified, because the operator's question is never "is
  // anything absent?" (on a converged database the answer is permanently yes) but
  // "is it the SAME set as last time?".
  ASSERT_EQ(rep->absent_cells.size(), std::size_t{1});
  EXPECT_EQ(rep->absent_cells.front().key, "2026-07-02");
  EXPECT_EQ(rep->absent_cells.front().symbol, "CCC");
  EXPECT_EQ(rep->n_absent_elided, std::size_t{0});

  // The counters still exhaust the walk, with absence as the fifth term.
  EXPECT_EQ(rep->cells_ok + rep->cells_absent + rep->cells_unmappable + rep->cells_non_finite +
                rep->cells_checksum,
            rep->cells_checked);

  // The symbol is untouched everywhere else — this is one missing CELL, not a
  // broken column, which is exactly the population that made the old verdict red.
  const auto other = db.map_surface("2026-07-01", "CCC");
  ASSERT_TRUE(other.has_value()) << (other ? "" : other.error().to_string());
}

// The two conditions in ONE database, told apart. This is the test that says the
// fix did its job: a report can carry both answers at once, they never contaminate
// each other's counter or list, and only the corruption moves the verdict.
TEST(SurfaceDbAdmin, VerifyDbSeparatesAbsentCellsFromCorruptOnes) {
  const AdminFixture f = make_fixture("verify_absent_vs_corrupt");
  build_healthy_db(f);
  drop_cell(f, "2026-07-02", "CCC");                    // never-stored: not a defect
  edit_record(f, "2026-07-01", "AAA", [](std::span<std::byte> rec) {
    rec.back() ^= std::byte{0xFF};                      // stored and now rotten: a defect
  });
  const SurfaceDb db = open_db(f);

  const auto rep = verify_db(db, DbVerifySpec{});
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_FALSE(rep->ok()) << "absence must not launder a checksum fault into a clean verdict";
  EXPECT_EQ(rep->cells_checked, std::size_t{9});
  EXPECT_EQ(rep->cells_ok, std::size_t{7});
  EXPECT_EQ(rep->cells_absent, std::size_t{1});
  EXPECT_EQ(rep->cells_checksum, std::size_t{1});
  EXPECT_EQ(rep->cells_unmappable, std::size_t{0});

  // Each on its own list, each naming its own cell, neither carrying the other's.
  ASSERT_EQ(rep->failures.size(), std::size_t{1});
  EXPECT_EQ(rep->failures.front().key, "2026-07-01");
  EXPECT_EQ(rep->failures.front().symbol, "AAA");
  EXPECT_EQ(rep->failures.front().kind, DbCellFailure::ChecksumMismatch);
  ASSERT_EQ(rep->absent_cells.size(), std::size_t{1});
  EXPECT_EQ(rep->absent_cells.front().key, "2026-07-02");
  EXPECT_EQ(rep->absent_cells.front().symbol, "CCC");

  // Removing the corruption alone flips the verdict, with the absence unmoved:
  // the two are independent inputs, not one blended "something is missing" score.
  const AdminFixture clean = make_fixture("verify_absent_only");
  build_healthy_db(clean);
  drop_cell(clean, "2026-07-02", "CCC");
  const SurfaceDb clean_db = open_db(clean);
  const auto clean_rep = verify_db(clean_db, DbVerifySpec{});
  ASSERT_TRUE(clean_rep.has_value()) << (clean_rep ? "" : clean_rep.error().to_string());
  EXPECT_TRUE(clean_rep->ok());
  EXPECT_EQ(clean_rep->cells_absent, rep->cells_absent);
}

// The absent list obeys the cap, and the cap's budget is its OWN. A shared pool
// would let absence — permanently non-zero on a converged database, and recorded
// first — elide the corruption fault that appeared beside it, which is the one
// line in the whole report that must never be the one that gets dropped.
TEST(SurfaceDbAdmin, VerifyDbAbsentListCapIsSeparateFromTheFailureCap) {
  const AdminFixture f = make_fixture("verify_absent_cap");
  build_healthy_db(f);
  for (const char *key : {"2026-07-01", "2026-07-02", "2026-07-06"}) {
    drop_cell(f, key, "CCC"); // three absences, one per date
  }
  edit_record(f, "2026-07-01", "AAA",
              [](std::span<std::byte> rec) { rec.back() ^= std::byte{0xFF}; });
  const SurfaceDb db = open_db(f);

  DbVerifySpec spec;
  spec.max_reported_failures = 1; // one number, spent twice
  const auto rep = verify_db(db, spec);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_FALSE(rep->ok());
  EXPECT_EQ(rep->cells_absent, std::size_t{3}); // totals are never truncated
  EXPECT_EQ(rep->cells_checksum, std::size_t{1});

  // The fault survived three absences competing for the same numeric cap.
  ASSERT_EQ(rep->failures.size(), std::size_t{1});
  EXPECT_EQ(rep->failures.front().kind, DbCellFailure::ChecksumMismatch);
  EXPECT_EQ(rep->n_failures_elided, std::size_t{0});
  // ...and the absent list truncated on its own budget, saying so.
  EXPECT_EQ(rep->absent_cells.size(), std::size_t{1});
  EXPECT_EQ(rep->n_absent_elided, std::size_t{2});

  // A zero cap retains no detail at all on either list and elides everything --
  // the counters still tell the truth (the `max_reported_failures == 0` contract).
  DbVerifySpec none;
  none.max_reported_failures = 0;
  const auto quiet = verify_db(db, none);
  ASSERT_TRUE(quiet.has_value()) << (quiet ? "" : quiet.error().to_string());
  EXPECT_TRUE(quiet->absent_cells.empty());
  EXPECT_EQ(quiet->n_absent_elided, std::size_t{3});
  EXPECT_EQ(quiet->cells_absent, std::size_t{3});
  EXPECT_TRUE(quiet->failures.empty());
  EXPECT_EQ(quiet->n_failures_elided, std::size_t{1});
}

// The walk read cells and the database held NOT ONE of them. `ok()` is true and
// must stay true — a deliberate narrowing onto known-absent cells has this exact
// shape and is a correct answer — but nothing else in the report can see the
// condition, so it gets its own predicate for the CLI to warn on.
//
// It is the gap FIX-H opened and this closes: before, an all-absent walk arrived
// as `unmappable` and `verdict FAILED`. `--min-cells` cannot catch it (it counts
// the GRID, and a grid of pure holes is full-sized) and `selected_no_cells` cannot
// either (that walk was empty; this one was not).
TEST(SurfaceDbAdmin, VerifyDbStoredNoSelectedCellIsFlaggedWithoutFailing) {
  const AdminFixture f = make_fixture("verify_stored_nothing");
  build_healthy_db(f);
  const SurfaceDb db = open_db(f);

  // A ticker the manifest never configured — which `DbVerifySpec::symbols`
  // deliberately accepts, because asserting a name is NOT in a database is a
  // legitimate question. Every cell of that column is absent.
  DbVerifySpec unconfigured;
  unconfigured.symbols = {"ZZZZ"};
  const auto rep = verify_db(db, unconfigured);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_EQ(rep->cells_checked, std::size_t{3});
  EXPECT_EQ(rep->cells_ok, std::size_t{0});
  EXPECT_EQ(rep->cells_absent, std::size_t{3});
  EXPECT_TRUE(rep->stored_no_selected_cell());
  EXPECT_TRUE(rep->ok()) << "nothing failed a gate, so the verdict must stay clean";
  EXPECT_FALSE(rep->selected_no_cells()) << "the walk was not empty -- that is a different flag";

  // The healthy full walk is not this. Neither is a walk that stored SOME of what
  // it read, which is the boundary the predicate has to hold at.
  const auto full = verify_db(db, DbVerifySpec{});
  ASSERT_TRUE(full.has_value()) << (full ? "" : full.error().to_string());
  EXPECT_FALSE(full->stored_no_selected_cell());

  const AdminFixture partial = make_fixture("verify_stored_something");
  build_healthy_db(partial);
  drop_cell(partial, "2026-07-02", "CCC");
  const SurfaceDb partial_db = open_db(partial);
  const auto some = verify_db(partial_db, DbVerifySpec{});
  ASSERT_TRUE(some.has_value()) << (some ? "" : some.error().to_string());
  EXPECT_EQ(some->cells_absent, std::size_t{1});
  EXPECT_FALSE(some->stored_no_selected_cell()) << "one hole is not an empty database";

  // DISJOINT FROM A FAILED VERDICT BY CONSTRUCTION: any cell that failed a gate is
  // a cell that is NOT absent, so `cells_absent == cells_checked` cannot hold
  // beside corruption. Proved rather than asserted in prose -- a whole date whose
  // file is gone is `unmappable`, not absent, so the predicate stays false while
  // the verdict is FAILED, and the CLI can never print both messages for one run.
  const AdminFixture broken = make_fixture("verify_stored_nothing_broken");
  build_healthy_db(broken);
  std::error_code ec;
  ASSERT_TRUE(fs::remove(partition_file(broken, "2026-07-02"), ec)) << ec.message();
  const SurfaceDb broken_db = open_db(broken);
  DbVerifySpec one_date;
  one_date.key_lo = "2026-07-02";
  one_date.key_hi = "2026-07-02";
  const auto dead = verify_db(broken_db, one_date);
  ASSERT_TRUE(dead.has_value()) << (dead ? "" : dead.error().to_string());
  EXPECT_EQ(dead->cells_checked, std::size_t{3});
  EXPECT_EQ(dead->cells_ok, std::size_t{0});
  EXPECT_EQ(dead->cells_unmappable, std::size_t{3});
  EXPECT_FALSE(dead->ok());
  EXPECT_FALSE(dead->stored_no_selected_cell())
      << "a walk that read nothing but FAULTS is a FAILED verdict, not this warning";
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

// A stored surface whose SMILE is degenerate (the total-variance intercept `a`
// of every SVI slice rewritten to a wildly negative constant) but whose record
// CRC is REPAIRED, so the bytes are perfectly self-consistent. S, r, and every
// slice's T/forward/qeff are all left untouched, so `PricedSurfaceView`'s
// semantic reconstruct-equivalence checks (cb7fe2e, [SE-P1-1]) — which reject a
// non-positive spot or forward at MAP time — stay silent: mapping proves
// nothing about it. Only the ATM evaluation, where `svi_total_w` goes negative
// and `sigma = sqrt(w/T)` is NaN, can tell it apart from a healthy cell. This is
// the one thing that makes `verify` more than a map-only walk, and it must be
// reported as its own failure kind, not as a checksum fault.
//
// (This fixture used to zero spot + every slice forward instead, which is what
// `map_surface` itself now rejects post-cb7fe2e — correctly: that commit closed
// exactly the reconstruct/view parity gap SE-P1-1 tracks. Corrupting the smile
// instead of the forward curve keeps this test on the ATM-probe path it exists
// to cover, without weakening the new, correct, earlier validation.)
TEST(SurfaceDbAdmin, VerifyDbFlagsNonFiniteAtmProbe) {
  const AdminFixture f = make_fixture("verify_nonfinite");
  build_healthy_db(f);
  edit_record(f, "2026-07-06", "BBB", [](std::span<std::byte> rec) {
    ArchiveV2SurfaceHeader h{};
    std::memcpy(&h, rec.data(), sizeof h);
    ASSERT_EQ(h.n_slices, 2u) << "fixture shape changed; the payload_off buffer below assumes 2";
    std::uint64_t payload_off[2] = {};
    std::memcpy(payload_off, rec.data() + h.col_payload_off_off, sizeof payload_off);
    for (std::uint32_t i = 0; i < h.n_slices; ++i) {
      // Must actually be Svi for `a` (SviParams's first field, byte 0 of the
      // per-slice payload) to land where this writes — assert it so a future
      // change to the fitted curve family fails loudly here instead of quietly
      // scribbling over an unrelated byte and making this test meaningless.
      const auto kind = static_cast<VolCurveKind>(
          *reinterpret_cast<const std::uint8_t *>(rec.data() + h.col_kind_off + i));
      ASSERT_EQ(kind, VolCurveKind::Svi);
    }
    // svi_total_w(k) == a + b*(rho*(k-m) + sqrt((k-m)^2 + sigma^2)); `a` alone
    // dominates for any realistically-fitted b/sigma, driving w negative at
    // every k — no need to reverse-engineer the fitted b/rho/m/sigma values.
    const double bogus_a = -1.0e9;
    for (const std::uint64_t off : payload_off) {
      std::memcpy(rec.data() + off, &bogus_a, sizeof bogus_a);
    }
    repair_record_crc(rec);
  });
  const SurfaceDb db = open_db(f);

  // The record is BYTE-VALID — the checksum branch must stay silent about it.
  const auto archive = db.open_partition("2026-07-06");
  ASSERT_TRUE(archive.has_value()) << (archive ? "" : archive.error().to_string());
  const Status crc = archive->validate_symbol("BBB");
  EXPECT_TRUE(crc.has_value()) << (crc ? "" : crc.error().to_string());

  // It still maps, and the forward curve is untouched — the failure is only
  // reachable by EVALUATING the smile.
  const auto mapped = db.map_surface("2026-07-06", "BBB");
  ASSERT_TRUE(mapped.has_value()) << (mapped ? "" : mapped.error().to_string());
  const double fwd = mapped->view.forward_at(kSurfaceDbVerifyProbeT);
  ASSERT_TRUE(std::isfinite(fwd) && fwd > 0.0)
      << "fwd=" << fwd << " — the forward curve must stay healthy; only the smile is corrupted";
  const double iv = mapped->view.iv(fwd, kSurfaceDbVerifyProbeT);
  EXPECT_FALSE(std::isfinite(iv) && iv > 0.0) << "iv=" << iv;

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

// ── the zero-spot ruling (closeout 1.4) ─────────────────────────────────────

// RULING: reject-and-reflag, not map-with-quarantine. `cb7fe2e` (SE-P1-1) made
// `PricedSurfaceView` refuse a non-positive spot, and that is the correctness
// stance: a surface with S <= 0 has no forward, so every number served off it
// is meaningless. A stored record carrying one must therefore be QUARANTINED by
// `verify_db` — counted `Unmappable`, named in the failure list, and refused by
// `query_surface` — never mapped and served with a warning.
//
// Nothing pinned that. The ATM-probe fixture above USED to be the zero-spot
// case and was rewritten (correctly) to corrupt the smile instead, so it could
// keep covering the ATM-probe branch; that left the zero-spot behaviour itself
// asserted nowhere, and a regression that re-admitted S <= 0 at map time would
// have been silent. This is the missing pin.
TEST(SurfaceDbAdmin, VerifyDbQuarantinesZeroSpotRecord) {
  const AdminFixture f = make_fixture("verify_zero_spot");
  build_healthy_db(f);
  edit_record(f, "2026-07-06", "BBB", [](std::span<std::byte> rec) {
    ArchiveV2SurfaceHeader h{};
    std::memcpy(&h, rec.data(), sizeof h);
    ASSERT_GT(h.S, 0.0) << "the fixture must start from a healthy spot";
    const double zero_spot = 0.0;
    std::memcpy(rec.data() + offsetof(ArchiveV2SurfaceHeader, S), &zero_spot, sizeof zero_spot);
    repair_record_crc(rec);
  });
  const SurfaceDb db = open_db(f);

  // BYTE-VALID: the CRC is repaired, so this is not a corruption story and the
  // checksum branch must stay silent about it.
  const auto archive = db.open_partition("2026-07-06");
  ASSERT_TRUE(archive.has_value()) << (archive ? "" : archive.error().to_string());
  const Status crc = archive->validate_symbol("BBB");
  EXPECT_TRUE(crc.has_value()) << (crc ? "" : crc.error().to_string());

  // ...and REJECTED at map time. This is the ruling: the view refuses to exist,
  // rather than existing and answering with a flag.
  const auto mapped = db.map_surface("2026-07-06", "BBB");
  EXPECT_FALSE(mapped.has_value()) << "S <= 0 must not map (cb7fe2e / SE-P1-1)";

  const auto rep = verify_db(db, DbVerifySpec{});
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_FALSE(rep->ok());
  EXPECT_EQ(rep->cells_checked, std::size_t{9});
  EXPECT_EQ(rep->cells_ok, std::size_t{8});
  EXPECT_EQ(rep->cells_unmappable, std::size_t{1});
  EXPECT_EQ(rep->cells_non_finite, std::size_t{0}); // never reached the ATM probe
  EXPECT_EQ(rep->cells_checksum, std::size_t{0});   // the bytes are fine
  EXPECT_EQ(rep->cells_absent, std::size_t{0});     // it IS stored; it is unusable
  ASSERT_EQ(rep->failures.size(), std::size_t{1});
  EXPECT_EQ(rep->failures.front().key, "2026-07-06");
  EXPECT_EQ(rep->failures.front().symbol, "BBB");
  EXPECT_EQ(rep->failures.front().kind, DbCellFailure::Unmappable);
  EXPECT_FALSE(rep->failures.front().detail.empty()); // carries the rejection text

  // The spot check agrees with the walk.
  const auto q = query_surface(db, "2026-07-06", "BBB", 100.0, kSurfaceDbVerifyProbeT);
  EXPECT_FALSE(q.has_value()) << "query_surface must reject what verify_db quarantines";

  // Every other cell is untouched, and the counters exhaust the walk.
  const auto aaa = query_surface(db, "2026-07-06", "AAA",
                                 db.map_surface("2026-07-06", "AAA")->view.forward_at(
                                     kSurfaceDbVerifyProbeT),
                                 kSurfaceDbVerifyProbeT);
  EXPECT_TRUE(aaa.has_value()) << (aaa ? "" : aaa.error().to_string());
  EXPECT_EQ(rep->cells_ok + rep->cells_absent + rep->cells_unmappable + rep->cells_non_finite +
                rep->cells_checksum,
            rep->cells_checked);
}

// The OTHER half of the same branch (surface_db_admin.cpp's
// `!std::isfinite(forward) || forward <= 0.0 || !usable_iv(iv)`): a cell whose
// EVALUATED forward itself is non-finite, not just its smile. Post-cb7fe2e every
// per-slice forward is validated at map time (S>0, forward[i]>0, both finite —
// see the test above), so this cannot be reached by corrupting a column value
// directly; it has to come from something reconstruct-equivalence does NOT
// bound: the effective yield `qeff[i]`, required only to be FINITE
// (priced_surface_view.cpp's semantic-validation loop), never magnitude-checked.
//
// `qeff` only feeds `forward` through interp_forward's EXTRAPOLATION branches
// (`forward = S * exp((rate - qeff) * T)`, priced_surface_view.cpp). The
// INTERIOR branch (what the test above's probe_T lands in) computes forward as
// `interpolate_positive_log(fwd_lo, fwd_hi, alpha)` — a convex combination of
// two already-bounded logs (term_carry.hpp) that can never overflow — so this
// case deliberately probes OUTSIDE the fitted tenor bracket to reach the
// exp()-based formula the interior path never uses.
TEST(SurfaceDbAdmin, VerifyDbFlagsNonFiniteExtrapolatedForward) {
  const AdminFixture f = make_fixture("verify_nonfinite_fwd");
  build_healthy_db(f);
  double front_T = 0.0;
  edit_record(f, "2026-07-06", "BBB", [&front_T](std::span<std::byte> rec) {
    ArchiveV2SurfaceHeader h{};
    std::memcpy(&h, rec.data(), sizeof h);
    ASSERT_GT(h.n_slices, 0u);
    std::memcpy(&front_T, rec.data() + h.col_T_off, sizeof front_T);
    // Blow up the FRONT slice's qeff. exp((rate - qeff) * T) overflows to +inf
    // for any T bounded away from 0 once `qeff` is this large — no need to know
    // the fitted rate/qeff/T values, they are swamped either way.
    const double bogus_qeff = -1.0e10;
    std::memcpy(rec.data() + h.col_qeff_off, &bogus_qeff, sizeof bogus_qeff);
    repair_record_crc(rec);
  });
  const SurfaceDb db = open_db(f);

  // BYTE-VALID and MAPS cleanly — qeff's magnitude is invisible to both the
  // checksum and the reconstruct-equivalence parse check.
  const auto archive = db.open_partition("2026-07-06");
  ASSERT_TRUE(archive.has_value()) << (archive ? "" : archive.error().to_string());
  const Status crc = archive->validate_symbol("BBB");
  EXPECT_TRUE(crc.has_value()) << (crc ? "" : crc.error().to_string());
  const auto mapped = db.map_surface("2026-07-06", "BBB");
  ASSERT_TRUE(mapped.has_value()) << (mapped ? "" : mapped.error().to_string());

  // Probing EXACTLY the front slice's own T takes interp_forward's exact-match
  // return (the stored, untouched forward column — no exp() involved) and stays
  // healthy...
  const double fwd_at_node = mapped->view.forward_at(front_T);
  EXPECT_TRUE(std::isfinite(fwd_at_node) && fwd_at_node > 0.0) << "fwd_at_node=" << fwd_at_node;
  // ...but anything SHORTER than the front slice extrapolates through the
  // corrupted qeff and overflows.
  const double probe_T = front_T / 2.0;
  const double fwd = mapped->view.forward_at(probe_T);
  EXPECT_FALSE(std::isfinite(fwd) && fwd > 0.0) << "fwd=" << fwd;

  DbVerifySpec spec;
  spec.probe_T = probe_T; // steer verify_db's own ATM probe into the same extrapolation
  const auto rep = verify_db(db, spec);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_FALSE(rep->ok());
  EXPECT_EQ(rep->cells_checked, std::size_t{9});
  EXPECT_EQ(rep->cells_ok, std::size_t{8});
  EXPECT_EQ(rep->cells_unmappable, std::size_t{0});
  EXPECT_EQ(rep->cells_non_finite, std::size_t{1});
  EXPECT_EQ(rep->cells_checksum, std::size_t{0}); // the bytes are fine; the FORWARD is not
  ASSERT_EQ(rep->failures.size(), std::size_t{1});
  EXPECT_EQ(rep->failures.front().key, "2026-07-06");
  EXPECT_EQ(rep->failures.front().symbol, "BBB");
  EXPECT_EQ(rep->failures.front().kind, DbCellFailure::NonFinite);
  EXPECT_FALSE(rep->failures.front().detail.empty());

  // query_surface agrees: it must not print a number for what verify_db rejects.
  const auto q = query_surface(db, "2026-07-06", "BBB", 100.0, probe_T);
  ASSERT_FALSE(q.has_value()) << "query_surface must reject what verify_db rejects";
  EXPECT_EQ(q.error().code(), ErrorCode::Internal);
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

// ── set_symbol_enabled — the one management action (FIX-G) ──────────────────
//
// The build CLI's carry-masked warning, and the manual before it, name exactly
// one remedy for a permanently-failing name: disable it. Until this call existed
// that remedy was a C++ API call reachable from no shipped tool. What follows
// pins the call itself, and then pins END TO END the invariant the two commits
// before it exist to protect — `enabled = false` means STOP FITTING, never
// DELETE — because an operator who cannot trust that will not use the remedy.

// A build restricted to a subset of the fixture's symbols and dates, so a later
// build can WIDEN it and thereby trigger a real rewrite (a new symbol on an
// existing date) or a real new fit (a new date).
[[nodiscard]] SurfaceDbBuildSpec
narrowed_spec(const AdminFixture &f, std::vector<std::string> symbols, std::size_t n_dates) {
  SurfaceDbBuildSpec spec = build_spec(f);
  spec.hive.symbols = std::move(symbols);
  spec.hive.date_lo = f.fx.dates.front();
  spec.hive.date_hi = f.fx.dates.at(n_dates - 1);
  return spec;
}

// The exact on-disk bytes of one stored surface record, read through the
// archive's own directory. This is the only oracle that distinguishes PRESERVED
// from RE-EMITTED-AFTER-A-REFIT: a re-fit under any config satisfies
// `load_surface` while silently changing the stored values.
[[nodiscard]] std::vector<std::byte>
stored_record_bytes(const AdminFixture &f, std::string_view key, std::string_view symbol) {
  const RecordExtent ext = record_extent(f, key, symbol);
  std::vector<std::byte> out;
  if (ext.size == 0) {
    return out;
  }
  std::ifstream in(partition_file(f, key), std::ios::binary);
  EXPECT_TRUE(in.good());
  if (!in.good()) {
    return out;
  }
  out.resize(static_cast<std::size_t>(ext.size));
  in.seekg(static_cast<std::streamoff>(ext.offset), std::ios::beg);
  in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(ext.size));
  EXPECT_EQ(static_cast<std::uint64_t>(in.gcount()), ext.size);
  return out;
}

// The whole point of routing this through `upsert_symbol` rather than writing a
// second manifest writer: one field moves and the rest of the stored config —
// which may be an operator's tuned override — comes back byte-for-byte.
TEST(SurfaceDbAdmin, SetSymbolEnabledMovesOneFieldAndIsIdempotent) {
  const AdminFixture f = make_fixture("set_enabled_one_field");
  build_healthy_db(f);
  SurfaceDb db = open_db(f);

  const std::vector<std::string> just_bbb{"BBB"};
  const auto before = db.symbol_config("BBB");
  ASSERT_TRUE(before.has_value()) << (before ? "" : before.error().to_string());
  ASSERT_TRUE(before->enabled);
  const auto prov_before = db.surface_provenance("BBB");
  ASSERT_TRUE(prov_before.has_value()) << (prov_before ? "" : prov_before.error().to_string());
  const std::uint64_t gen_before = db.generation();
  // The oracle for "nothing else moved" is the PUBLIC fold: `config_fingerprint`
  // digests the whole 256-byte encoded config (surface_policy included) with only
  // the provenance half zeroed. Comparing structs field by field would miss a
  // knob `SymbolFitConfig` grows later; this cannot.
  const std::uint64_t fp_before = db.config_fingerprint(just_bbb);
  ASSERT_NE(fp_before, 0u) << "0 is the unknown sentinel; a configured symbol must fold";

  const auto off = set_symbol_enabled(db, "bbb", false); // case-insensitive, like every lookup
  ASSERT_TRUE(off.has_value()) << (off ? "" : off.error().to_string());
  EXPECT_EQ(off->symbol, "BBB"); // canonical spelling echoed back
  EXPECT_TRUE(off->was_enabled);
  EXPECT_FALSE(off->now_enabled);
  EXPECT_TRUE(off->changed);
  EXPECT_GT(off->generation, gen_before) << "a real change must persist and bump the generation";

  const auto after = db.symbol_config("BBB");
  ASSERT_TRUE(after.has_value()) << (after ? "" : after.error().to_string());
  EXPECT_FALSE(after->enabled);
  // The knobs an operator reads back are unchanged while it is off.
  EXPECT_EQ(after->preset, before->preset);
  EXPECT_EQ(after->pin_curve, before->pin_curve);
  EXPECT_EQ(after->curve.kind, before->curve.kind);
  EXPECT_DOUBLE_EQ(after->band_k, before->band_k);
  EXPECT_EQ(after->surface_policy.quality_mode, before->surface_policy.quality_mode);
  EXPECT_EQ(after->surface_policy.risk_admission, before->surface_policy.risk_admission);
  // `enabled` IS in the fold, which is why disabling a name invalidates the
  // carry-over fingerprint of every partition holding it — the mechanism
  // `DisabledSymbolStoredSurfaceSurvivesARewrite` depends on. Pinned so a future
  // change that drops it from the fold is caught here too.
  EXPECT_NE(db.config_fingerprint(just_bbb), fp_before)
      << "`enabled` must be part of the config fold";

  // Provenance survives. `upsert_symbol(sym, cfg)` defaults its provenance
  // argument to nullopt, which for an EXISTING symbol means KEEP — if that ever
  // changed to "clear", `config --symbol` would start reporting provenance 0
  // after a disable and the manifest would have lost the fit's own record of
  // itself.
  const auto prov_after = db.surface_provenance("BBB");
  ASSERT_TRUE(prov_after.has_value()) << (prov_after ? "" : prov_after.error().to_string());
  EXPECT_EQ(prov_before->has_value(), prov_after->has_value());

  // Idempotent, and a no-op writes NOTHING: a converging operator script may
  // assert the desired state on every run without churning the manifest.
  const std::uint64_t gen_off = db.generation();
  const auto again = set_symbol_enabled(db, "BBB", false);
  ASSERT_TRUE(again.has_value()) << (again ? "" : again.error().to_string());
  EXPECT_FALSE(again->changed);
  EXPECT_FALSE(again->was_enabled);
  EXPECT_FALSE(again->now_enabled);
  EXPECT_EQ(again->generation, gen_off) << "a no-op must not rewrite the manifest";

  // ...and back. The fold returning to its ORIGINAL value is the proof that the
  // disable moved one bit and nothing else: had it zeroed or defaulted any other
  // field, re-enabling would not restore it and this would differ.
  const auto on = set_symbol_enabled(db, "BBB", true);
  ASSERT_TRUE(on.has_value()) << (on ? "" : on.error().to_string());
  EXPECT_TRUE(on->changed);
  EXPECT_TRUE(on->now_enabled);
  EXPECT_EQ(db.config_fingerprint(just_bbb), fp_before)
      << "a disable/enable round trip must return the stored config to its original bytes";
}

// A name the manifest does not configure is NotFound, and nothing is written.
// The alternative — `upsert_symbol` inventing a default config for it — would
// turn a typo into a silently-created disabled symbol that no build ever fixes.
TEST(SurfaceDbAdmin, SetSymbolEnabledOnAnUnconfiguredSymbolIsNotFoundAndWritesNothing) {
  const AdminFixture f = make_fixture("set_enabled_unknown");
  build_healthy_db(f);
  SurfaceDb db = open_db(f);

  const std::uint64_t gen_before = db.generation();
  const auto miss = set_symbol_enabled(db, "ZZZ", false);
  ASSERT_FALSE(miss.has_value());
  EXPECT_EQ(miss.error().code(), ErrorCode::NotFound);
  EXPECT_EQ(db.generation(), gen_before) << "a failed lookup must not have written the manifest";
  EXPECT_EQ(db.symbols(), (std::vector<std::string>{"AAA", "BBB", "CCC"}))
      << "no symbol may have been created";
}

// ── THE INVARIANT, END TO END ───────────────────────────────────────────────
//
// A disable performed through the shipped path must not cost the symbol its
// stored surfaces. This drives the WHOLE stack the operator drives — the same
// `set_symbol_enabled` the CLI's `disable` calls, then `build_surface_db`, the
// one-call driver behind `atx-vol-surface-db-build` — and compares the stored
// record BYTES either side of a rewrite that really happened.
//
// The rewrite has to be real or the test passes vacuously: build 1 covers
// {AAA, BBB} on two dates, and build 2 widens the universe to {AAA, BBB, CCC} on
// the SAME two dates, so CCC is a genuinely new cell on each, each date's
// partition is rewritten from scratch, and BBB's bytes have to survive that
// rewrite rather than merely never be touched. That is exactly the unrelated
// trigger FIX-E's defect fired on.
TEST(SurfaceDbAdmin, DisableThenRebuildPreservesTheStoredSurfaceBytes) {
  const AdminFixture f = make_fixture("disable_preserves_bytes");
  const std::vector<std::string> dates{f.fx.dates[0], f.fx.dates[1]};

  // Build 1: AAA and BBB over two dates. BBB must be STORED before it is
  // disabled, or nothing is at risk and the test proves nothing.
  {
    const auto first = build_surface_db(narrowed_spec(f, {"AAA", "BBB"}, 2));
    ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
    ASSERT_EQ(first->coverage.cells_ok, 4u) << "2 symbols x 2 dates must have fitted";
    ASSERT_EQ(first->coverage.dates_written, 2u);
  }
  std::vector<std::vector<std::byte>> bbb_before;
  for (const std::string &d : dates) {
    bbb_before.push_back(stored_record_bytes(f, d, "BBB"));
    ASSERT_FALSE(bbb_before.back().empty()) << d;
  }

  // The operator action, through the exact call `atx-vol-surface-db disable` makes.
  {
    SurfaceDb db = open_db(f);
    const auto ch = set_symbol_enabled(db, "BBB", false);
    ASSERT_TRUE(ch.has_value()) << (ch ? "" : ch.error().to_string());
    ASSERT_TRUE(ch->changed);
    ASSERT_FALSE(ch->now_enabled);
  }

  // Build 2: the unrelated trigger. CCC is new on both dates, so both partitions
  // are rewritten. Nothing about CCC concerns BBB.
  const auto second = build_surface_db(narrowed_spec(f, {"AAA", "BBB", "CCC"}, 2));
  ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());
  ASSERT_EQ(second->coverage.dates_written, 2u)
      << "both dates must really have been rewritten, or the bytes survived by not being touched";
  ASSERT_EQ(second->coverage.dates_skipped_would_drop, 0u);
  EXPECT_EQ(second->coverage.cells_carried_disabled, 2u)
      << "BBB's two cells must be counted as PRESERVED";
  EXPECT_EQ(second->config.n_disabled_existing, 1u) << "the build must still NAME the disabled BBB";

  // The disable took effect on the FIT side: BBB was not offered to the fitter.
  const auto bbb_stats =
      std::find_if(second->coverage.per_symbol.begin(), second->coverage.per_symbol.end(),
                   [](const PopulateSymbolStats &s) { return s.symbol == "BBB"; });
  ASSERT_NE(bbb_stats, second->coverage.per_symbol.end());
  EXPECT_EQ(bbb_stats->n_ok, 0u) << "a disabled symbol must not be fitted";
  EXPECT_EQ(bbb_stats->n_carried, 0u) << "a preserved cell is not a healthy carried one";

  // ── The bite: byte-identical, on every date. ──────────────────────────────
  for (std::size_t i = 0; i < dates.size(); ++i) {
    const std::vector<std::byte> after = stored_record_bytes(f, dates[i], "BBB");
    ASSERT_EQ(bbb_before[i].size(), after.size()) << dates[i];
    EXPECT_EQ(0, std::memcmp(bbb_before[i].data(), after.data(), after.size()))
        << "disabling BBB changed its stored surface on " << dates[i]
        << " -- `enabled = false` must mean STOP FITTING, never DELETE or REWRITE";
  }

  // And the surfaces are not merely present bytes: they still map, checksum and
  // evaluate, through the same walk an operator runs. The default walk drops the
  // disabled column, so this is the `--include-disabled` case, and `verify` names
  // BBB as a column it would otherwise not have looked at.
  const SurfaceDb db = open_db(f);
  DbVerifySpec forced;
  forced.include_disabled = true;
  const auto all = verify_db(db, forced);
  ASSERT_TRUE(all.has_value()) << (all ? "" : all.error().to_string());
  EXPECT_EQ(all->cells_checked, std::size_t{6}); // 3 symbols x 2 dates
  EXPECT_TRUE(all->ok()) << "a preserved disabled surface must still be a HEALTHY cell";

  const auto defaulted = verify_db(db, DbVerifySpec{});
  ASSERT_TRUE(defaulted.has_value()) << (defaulted ? "" : defaulted.error().to_string());
  EXPECT_EQ(defaulted->disabled_symbols, (std::vector<std::string>{"BBB"}));

  fs::remove_all(f.root);
}

// The round trip. Disable -> rebuild -> enable -> rebuild returns BBB to normal
// fitting, and everything it held before the disable is still there afterwards.
//
// Build 3 WIDENS the date window rather than re-running the same one, because a
// resume over an unchanged window is `dates_skipped_complete` and would prove
// only that nothing happened. The new date is a cell BBB has never fitted, so
// "BBB fits again" is observed, not inferred.
TEST(SurfaceDbAdmin, DisableEnableRoundTripRefitsTheSymbolWithNoDataLost) {
  const AdminFixture f = make_fixture("disable_enable_round_trip");
  const std::vector<std::string> old_dates{f.fx.dates[0], f.fx.dates[1]};
  const std::string new_date = f.fx.dates[2];

  {
    const auto first = build_surface_db(narrowed_spec(f, {"AAA", "BBB"}, 2));
    ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
    ASSERT_EQ(first->coverage.cells_ok, 4u);
  }
  std::vector<std::vector<std::byte>> bbb_before;
  for (const std::string &d : old_dates) {
    bbb_before.push_back(stored_record_bytes(f, d, "BBB"));
    ASSERT_FALSE(bbb_before.back().empty()) << d;
  }
  const std::vector<std::string> just_bbb{"BBB"};
  const std::uint64_t fp_before = open_db(f).config_fingerprint(just_bbb);
  ASSERT_NE(fp_before, 0u);

  { // disable
    SurfaceDb db = open_db(f);
    const auto ch = set_symbol_enabled(db, "BBB", false);
    ASSERT_TRUE(ch.has_value()) << (ch ? "" : ch.error().to_string());
    ASSERT_TRUE(ch->changed);
  }
  { // rebuild over the same window, with CCC as the unrelated rewrite trigger
    const auto second = build_surface_db(narrowed_spec(f, {"AAA", "BBB", "CCC"}, 2));
    ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());
    ASSERT_EQ(second->coverage.dates_written, 2u);
    ASSERT_EQ(second->coverage.cells_carried_disabled, 2u);
  }
  { // enable
    SurfaceDb db = open_db(f);
    const auto ch = set_symbol_enabled(db, "BBB", true);
    ASSERT_TRUE(ch.has_value()) << (ch ? "" : ch.error().to_string());
    ASSERT_TRUE(ch->changed);
    ASSERT_TRUE(ch->now_enabled);
    // Re-enabling restores the config the operator disabled, not a fresh one.
    EXPECT_EQ(db.config_fingerprint(just_bbb), fp_before)
        << "the re-enabled config must be the one that was disabled, byte for byte";
  }

  // Build 3: the full window. The third date has never been written, so every
  // symbol is a new cell there -- including BBB, whose re-fit is the observation
  // this test exists for. `retry_disabled` is NOT set: an operator `enable` must
  // stand on its own without the build flag.
  const auto third = build_surface_db(narrowed_spec(f, {"AAA", "BBB", "CCC"}, 3));
  ASSERT_TRUE(third.has_value()) << (third ? "" : third.error().to_string());
  EXPECT_EQ(third->coverage.dates_written, 1u) << "only the new date should be written";
  EXPECT_EQ(third->coverage.cells_ok, 3u) << "all three symbols must fit on the new date";
  EXPECT_EQ(third->config.n_disabled_existing, 0u) << "nothing is disabled any more";
  EXPECT_TRUE(third->config.failed_symbols.empty());

  const auto bbb_stats =
      std::find_if(third->coverage.per_symbol.begin(), third->coverage.per_symbol.end(),
                   [](const PopulateSymbolStats &s) { return s.symbol == "BBB"; });
  ASSERT_NE(bbb_stats, third->coverage.per_symbol.end());
  EXPECT_EQ(bbb_stats->n_ok, 1u) << "BBB must be fitting normally again";
  EXPECT_EQ(bbb_stats->n_disabled, 0u);

  // NO DATA LOST across the whole round trip: the pre-disable bytes on the old
  // dates are still exactly the pre-disable bytes, and BBB now also holds the new
  // date's surface.
  for (std::size_t i = 0; i < old_dates.size(); ++i) {
    const std::vector<std::byte> after = stored_record_bytes(f, old_dates[i], "BBB");
    ASSERT_EQ(bbb_before[i].size(), after.size()) << old_dates[i];
    EXPECT_EQ(0, std::memcmp(bbb_before[i].data(), after.data(), after.size()))
        << "a disable/enable round trip lost BBB's stored surface on " << old_dates[i];
  }

  const SurfaceDb db = open_db(f);
  EXPECT_TRUE(db.load_surface(new_date, "BBB").has_value())
      << "BBB must have produced a surface on the date it was enabled for";
  const auto sym = describe_symbol(db, "BBB");
  ASSERT_TRUE(sym.has_value()) << (sym ? "" : sym.error().to_string());
  EXPECT_TRUE(sym->enabled);

  // The default walk sees BBB again -- no dropped column, and every cell healthy.
  const auto rep = verify_db(db, DbVerifySpec{});
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().to_string());
  EXPECT_TRUE(rep->disabled_symbols.empty());
  EXPECT_TRUE(rep->ok());
  EXPECT_EQ(rep->cells_checked, std::size_t{9}); // 3 symbols x 3 dates, nothing missing
  fs::remove_all(f.root);
}

} // namespace
} // namespace atx::vol
