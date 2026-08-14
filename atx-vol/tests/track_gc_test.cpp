// track_gc.hpp -- last_access_ts-driven retention/GC for the track lakehouse
// (Task D6, backtest-production-lakehouse sprint).
//
// Only built when ATX_VOL_LAKEHOUSE is ON (tests/CMakeLists.txt) -- needs
// Catalog and TrackStore::compact() actually compiled into atx-vol,
// mirroring catalog_test.cpp/track_compact_reconcile_test.cpp.
//
// Fixture helpers (make_result/make_registration/make_key/stage_one) are
// deliberately re-derived here rather than shared with
// track_compact_reconcile_test.cpp -- same call that file itself already
// made against track_store_test.cpp's own make_result (see its own top
// comment): each Arrow+Catalog seam test file owns small, independent
// fixtures rather than growing a cross-file test-utility dependency.

#include "atx/vol/research/track_gc.hpp"

#include <arrow/array.h>
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/db/sqlite.hpp"
#include "atx/vol/api/backtest/backtest.hpp"             // BacktestResult
#include "marketdata/catalog.hpp"     // Catalog, TrackRow, TrackStatus
#include "storage/track_key.hpp"   // TrackKey
#include "storage/track_store.hpp" // TrackStore, TrackMeta, compact

using namespace atx::vol;
namespace fs = std::filesystem;
namespace db = atx::core::db;

namespace {

constexpr std::int64_t kBaseNow = 1767312000000000000LL; // 2026-01-02T00:00:00Z-ish, exact value unused
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;

[[nodiscard]] fs::path fresh_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-track-gc-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

[[nodiscard]] TrackKey make_key(std::uint8_t fill_byte) {
  TrackKey k;
  k.sha256.fill(fill_byte);
  return k;
}

[[nodiscard]] BacktestResult make_result(const std::vector<std::string> &dates, double base_value) {
  BacktestResult r;
  const std::size_t n = dates.size();
  r.date = dates;
  r.ts_ns.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    r.ts_ns[i] = kBaseNow + static_cast<std::int64_t>(i) * kDayNs;
  }
  const auto fill = [&](double offset) {
    std::vector<double> v(n);
    for (std::size_t i = 0; i < n; ++i) {
      v[i] = base_value + offset + static_cast<double>(i) * 0.001;
    }
    return v;
  };
  r.pnl_total = fill(1.0);
  r.pnl_delta = fill(2.0);
  r.pnl_gamma = fill(3.0);
  r.pnl_vega = fill(4.0);
  r.pnl_vanna = fill(5.0);
  r.pnl_volga = fill(6.0);
  r.pnl_theta = fill(7.0);
  r.pnl_rho = fill(8.0);
  r.pnl_charm = fill(9.0);
  r.pnl_unexplained = fill(10.0);
  r.pnl_settlement = fill(11.0);
  r.pnl_shares = fill(12.0);
  r.financing = fill(13.0);
  r.cost = fill(14.0);
  r.nav = fill(15.0);
  r.cash = fill(16.0);
  r.gross_delta = fill(17.0);
  r.gross_gamma = fill(18.0);
  r.gross_vega = fill(19.0);
  r.gross_theta = fill(20.0);
  r.turnover_notional = fill(21.0);
  r.turnover_vega = fill(22.0);
  r.n_open_lots = fill(23.0);
  r.n_unpriced_lots = fill(24.0);
  r.n_unpriced_greeks = fill(25.0);
  return r;
}

[[nodiscard]] TrackRegistration make_registration() {
  TrackRegistration reg;
  reg.config_json = "{}";
  reg.engine_id = "test-engine";
  reg.economics_rev = 1;
  reg.data_snapshot_id = "snap";
  reg.date_min = "2026-01-02";
  reg.date_max = "2026-01-04";
  return reg;
}

// Stages + registers ONE track (real write_staging + real register_staging).
[[nodiscard]] TrackKey stage_one(const fs::path &lake_root, Catalog &catalog, std::uint8_t key_fill,
                                 const std::string &underlier, const std::string &family) {
  const TrackKey key = make_key(key_fill);
  const TrackMeta meta{underlier, family};
  TrackStore store(lake_root.string());
  const BacktestResult result =
      make_result({"2026-01-02", "2026-01-03", "2026-01-04"}, 100.0 * static_cast<double>(key_fill));
  EXPECT_TRUE(store.write_staging(key, result, meta).has_value());
  EXPECT_TRUE(catalog.register_staging(key, meta, make_registration()).has_value());
  return key;
}

// The full, non-crashed track_compact pipeline: compact() (TrackStore-level,
// Catalog-agnostic -- see track_store.hpp's own doc comment) folds staging/
// into a batch, then EVERY placement it reports is mark_compacted()'d, the
// same two-step orchestration tools/track_compact.cpp's CLI performs. gc()
// only ever acts on 'compacted'/'retired' rows, so a test fixture that skips
// this second step (unlike track_compact_reconcile_test.cpp's OWN stage_one
// + bare compact(), which deliberately leaves rows 'staging' to simulate a
// stuck crash) would leave every row 'staging' and every gc() assertion
// below vacuously true.
void compact_and_mark(const fs::path &lake_root, Catalog &catalog) {
  auto compacted = compact(lake_root.string());
  ASSERT_TRUE(compacted.has_value()) << (compacted ? "" : compacted.error().to_string());
  for (const CompactedTrackPlacement &placement : compacted->placements) {
    auto key = track_key_from_hex(placement.track_key_hex);
    ASSERT_TRUE(key.has_value()) << (key ? "" : key.error().to_string());
    ASSERT_TRUE(catalog.mark_compacted(*key, placement.file, placement.row_group).has_value());
  }
}

// Test-only backdoor -- Catalog exposes no way to forge last_access_ts, so
// this reaches around it with a raw connection, same discipline
// catalog_test.cpp's own set_last_access_ts_raw uses.
void set_last_access_ts_raw(const fs::path &lake_root, const std::string &track_key, std::int64_t ts) {
  const fs::path db_path = lake_root / std::string(kCatalogDbName);
  auto conn = db::Database::open(db_path.string(), db::OpenMode::ReadWrite);
  ASSERT_TRUE(conn.has_value()) << (conn ? "" : conn.error().to_string());
  auto stmt = conn->prepare("UPDATE tracks SET last_access_ts = ?1 WHERE track_key = ?2;");
  ASSERT_TRUE(stmt.has_value());
  ASSERT_TRUE(stmt->bind(1, ts).has_value());
  ASSERT_TRUE(stmt->bind(2, track_key).has_value());
  auto step = stmt->step();
  ASSERT_TRUE(step.has_value()) << (step ? "" : step.error().to_string());
  ASSERT_EQ(conn->changes(), 1) << "set_last_access_ts_raw: no such track_key: " << track_key;
}

// Every value in a batch file's track_key column, for verifying WHICH tracks
// survived a rewrite -- same read shape as
// track_compact_reconcile.cpp::batch_contains_track, generalized to return
// every value instead of testing for one.
[[nodiscard]] std::vector<std::string> read_track_keys(const fs::path &path) {
  auto in = arrow::io::ReadableFile::Open(path.string());
  EXPECT_TRUE(in.ok()) << in.status().ToString();
  if (!in.ok()) {
    return {};
  }
  auto reader_res = parquet::arrow::OpenFile(*in, arrow::default_memory_pool());
  EXPECT_TRUE(reader_res.ok()) << reader_res.status().ToString();
  if (!reader_res.ok()) {
    return {};
  }
  std::unique_ptr<parquet::arrow::FileReader> reader = *std::move(reader_res);
  auto table_res = reader->ReadTable();
  EXPECT_TRUE(table_res.ok()) << table_res.status().ToString();
  if (!table_res.ok()) {
    return {};
  }
  auto combined = (*table_res)->CombineChunks(arrow::default_memory_pool());
  EXPECT_TRUE(combined.ok());
  const int idx = (*combined)->schema()->GetFieldIndex("track_key");
  EXPECT_GE(idx, 0);
  const auto &arr = static_cast<const arrow::StringArray &>(*(*combined)->column(idx)->chunk(0));
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(arr.length()));
  for (std::int64_t i = 0; i < arr.length(); ++i) {
    out.emplace_back(arr.GetString(i));
  }
  return out;
}

[[nodiscard]] std::int64_t raw_count(const fs::path &lake_root, std::string_view sql) {
  const fs::path db_path = lake_root / std::string(kCatalogDbName);
  auto conn = db::Database::open(db_path.string(), db::OpenMode::ReadOnly);
  EXPECT_TRUE(conn.has_value());
  auto stmt = conn->prepare(sql);
  EXPECT_TRUE(stmt.has_value());
  auto step = stmt->step();
  EXPECT_TRUE(step.has_value() && *step == db::Statement::Step::Row);
  return stmt->column_int(0);
}

} // namespace

TEST(TrackGcTest, RetiresOldCompactedTrackAndRewritesBatchDroppingItsRows) {
  const fs::path lake_root = fresh_dir("rewrite");
  auto catalog = Catalog::open(lake_root.string());
  ASSERT_TRUE(catalog.has_value());

  const TrackKey old_key = stage_one(lake_root, *catalog, 0x11, "SPY", "strangle");
  const TrackKey new_key = stage_one(lake_root, *catalog, 0x22, "SPY", "strangle");
  compact_and_mark(lake_root, *catalog);

  auto old_row_before = catalog->probe(old_key);
  ASSERT_TRUE(old_row_before.has_value() && old_row_before->has_value());
  const std::string original_file = *(*old_row_before)->file;

  set_last_access_ts_raw(lake_root, old_key.hex(), 1000);
  set_last_access_ts_raw(lake_root, new_key.hex(), 5000);

  auto stats = gc(lake_root.string(), 3000);
  ASSERT_TRUE(stats.has_value()) << (stats ? "" : stats.error().to_string());
  EXPECT_EQ(stats->tracks_retired, 1u);
  EXPECT_EQ(stats->batches_rewritten, 1u);
  EXPECT_EQ(stats->batches_deleted, 0u);
  EXPECT_EQ(stats->batches_skipped_live_reader, 0u);

  auto old_row = catalog->probe(old_key);
  ASSERT_TRUE(old_row.has_value() && old_row->has_value());
  EXPECT_EQ((*old_row)->status, TrackStatus::Retired);
  EXPECT_FALSE((*old_row)->file.has_value()) << "reclaimed -- its rows were dropped from the rewrite";

  auto new_row = catalog->probe(new_key);
  ASSERT_TRUE(new_row.has_value() && new_row->has_value());
  EXPECT_EQ((*new_row)->status, TrackStatus::Compacted);
  ASSERT_TRUE((*new_row)->file.has_value());
  EXPECT_NE(*(*new_row)->file, original_file) << "the batch was rewritten at a NEW filename";
  ASSERT_TRUE((*new_row)->row_group.has_value());
  EXPECT_EQ(*(*new_row)->row_group, 0);

  const fs::path original_path = lake_root / original_file;
  EXPECT_FALSE(fs::exists(original_path)) << "old file removed after the catalog was repointed";

  const fs::path new_path = lake_root / *(*new_row)->file;
  ASSERT_TRUE(fs::exists(new_path));
  const std::vector<std::string> keys_in_new_file = read_track_keys(new_path);
  EXPECT_EQ(std::count(keys_in_new_file.begin(), keys_in_new_file.end(), new_key.hex()),
            static_cast<std::ptrdiff_t>(3))
      << "new_key's 3 rows must have survived the rewrite";
  EXPECT_EQ(std::count(keys_in_new_file.begin(), keys_in_new_file.end(), old_key.hex()), 0)
      << "old_key's rows must NOT be present in the rewritten file";
}

TEST(TrackGcTest, DeletesBatchFileWhenEveryTrackInItIsRetired) {
  const fs::path lake_root = fresh_dir("delete");
  auto catalog = Catalog::open(lake_root.string());
  ASSERT_TRUE(catalog.has_value());

  const TrackKey key = stage_one(lake_root, *catalog, 0x33, "QQQ", "iron_condor");
  compact_and_mark(lake_root, *catalog);
  auto row_before = catalog->probe(key);
  ASSERT_TRUE(row_before.has_value() && row_before->has_value());
  const fs::path original_path = lake_root / *(*row_before)->file;
  ASSERT_TRUE(fs::exists(original_path));

  set_last_access_ts_raw(lake_root, key.hex(), 1000);
  auto stats = gc(lake_root.string(), 3000);
  ASSERT_TRUE(stats.has_value()) << (stats ? "" : stats.error().to_string());
  EXPECT_EQ(stats->tracks_retired, 1u);
  EXPECT_EQ(stats->batches_rewritten, 0u);
  EXPECT_EQ(stats->batches_deleted, 1u);

  auto row = catalog->probe(key);
  ASSERT_TRUE(row.has_value() && row->has_value());
  EXPECT_EQ((*row)->status, TrackStatus::Retired);
  EXPECT_FALSE((*row)->file.has_value());
  EXPECT_FALSE(fs::exists(original_path)) << "the whole batch was reclaimed -- nothing survived it";
}

TEST(TrackGcTest, NeverTouchesAFileWithALiveReaderMarkThenReclaimsAfterRelease) {
  const fs::path lake_root = fresh_dir("live-reader");
  auto catalog = Catalog::open(lake_root.string());
  ASSERT_TRUE(catalog.has_value());

  const TrackKey key = stage_one(lake_root, *catalog, 0x44, "SPY", "strangle");
  compact_and_mark(lake_root, *catalog);
  auto row_before = catalog->probe(key);
  ASSERT_TRUE(row_before.has_value() && row_before->has_value());
  const std::string file = *(*row_before)->file;
  const fs::path original_path = lake_root / file;

  set_last_access_ts_raw(lake_root, key.hex(), 1000);

  auto mark_id = catalog->mark_reader(file);
  ASSERT_TRUE(mark_id.has_value()) << (mark_id ? "" : mark_id.error().to_string());

  auto first = gc(lake_root.string(), 3000);
  ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
  EXPECT_EQ(first->tracks_retired, 1u) << "retiring in the catalog is always safe -- never gated by a mark";
  EXPECT_EQ(first->batches_rewritten, 0u);
  EXPECT_EQ(first->batches_deleted, 0u);
  EXPECT_EQ(first->batches_skipped_live_reader, 1u);

  auto row_marked = catalog->probe(key);
  ASSERT_TRUE(row_marked.has_value() && row_marked->has_value());
  EXPECT_EQ((*row_marked)->status, TrackStatus::Retired);
  ASSERT_TRUE((*row_marked)->file.has_value())
      << "a live-marked file must be left completely untouched -- file pointer unchanged";
  EXPECT_EQ(*(*row_marked)->file, file);
  EXPECT_TRUE(fs::exists(original_path)) << "the marked file itself must survive";

  ASSERT_TRUE(catalog->release_reader_mark(*mark_id).has_value());
  auto second = gc(lake_root.string(), 3000);
  ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());
  EXPECT_EQ(second->tracks_retired, 0u) << "already retired by the first call";
  EXPECT_EQ(second->batches_deleted, 1u) << "now reclaimable -- the mark was released";

  auto row_after = catalog->probe(key);
  ASSERT_TRUE(row_after.has_value() && row_after->has_value());
  EXPECT_FALSE((*row_after)->file.has_value());
  EXPECT_FALSE(fs::exists(original_path));
}

TEST(TrackGcTest, NeverRetiresTracksNewerThanTheThreshold) {
  const fs::path lake_root = fresh_dir("too-new");
  auto catalog = Catalog::open(lake_root.string());
  ASSERT_TRUE(catalog.has_value());

  const TrackKey key = stage_one(lake_root, *catalog, 0x55, "SPY", "strangle");
  compact_and_mark(lake_root, *catalog);
  set_last_access_ts_raw(lake_root, key.hex(), 9000);

  auto stats = gc(lake_root.string(), 3000);
  ASSERT_TRUE(stats.has_value()) << (stats ? "" : stats.error().to_string());
  EXPECT_EQ(stats->tracks_retired, 0u);
  EXPECT_EQ(stats->batches_rewritten, 0u);
  EXPECT_EQ(stats->batches_deleted, 0u);

  auto row = catalog->probe(key);
  ASSERT_TRUE(row.has_value() && row->has_value());
  EXPECT_EQ((*row)->status, TrackStatus::Compacted);
}

TEST(TrackGcTest, IsIdempotentOnRerunWithNothingLeftToDo) {
  const fs::path lake_root = fresh_dir("idempotent");
  auto catalog = Catalog::open(lake_root.string());
  ASSERT_TRUE(catalog.has_value());

  const TrackKey old_key = stage_one(lake_root, *catalog, 0x66, "SPY", "strangle");
  const TrackKey new_key = stage_one(lake_root, *catalog, 0x77, "SPY", "strangle");
  compact_and_mark(lake_root, *catalog);
  set_last_access_ts_raw(lake_root, old_key.hex(), 1000);
  set_last_access_ts_raw(lake_root, new_key.hex(), 5000);

  auto first = gc(lake_root.string(), 3000);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->tracks_retired, 1u);
  EXPECT_EQ(first->batches_rewritten, 1u);

  auto second = gc(lake_root.string(), 3000);
  ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());
  EXPECT_EQ(second->tracks_retired, 0u);
  EXPECT_EQ(second->batches_rewritten, 0u);
  EXPECT_EQ(second->batches_deleted, 0u);
  EXPECT_EQ(second->batches_skipped_live_reader, 0u)
      << "the already-reclaimed row's file is NULL -- it is no longer an affected file at all";
}

// Simulates the crash window the brief asks for, mirroring D5's own
// discipline (track_compact_reconcile_test.cpp's StuckStagingRowIsRelocated
// AndMarkedCompacted): rather than actually killing a process mid-gc(),
// plant the exact ON-DISK STATE a crash between "new file durably published"
// and "catalog transaction committed" would leave behind -- a pre-existing
// file at the SAME name gc()'s own numbering would pick next -- and prove
// gc() converges anyway: it must not collide with, or overwrite, the orphan.
TEST(TrackGcTest, PreexistingOrphanGcBatchFromASimulatedCrashDoesNotBlockOrGetOverwritten) {
  const fs::path lake_root = fresh_dir("orphan");
  auto catalog = Catalog::open(lake_root.string());
  ASSERT_TRUE(catalog.has_value());

  const TrackKey old_key = stage_one(lake_root, *catalog, 0x88, "SPY", "strangle");
  const TrackKey new_key = stage_one(lake_root, *catalog, 0x99, "SPY", "strangle");
  compact_and_mark(lake_root, *catalog);
  auto row_before = catalog->probe(old_key);
  ASSERT_TRUE(row_before.has_value() && row_before->has_value());
  const fs::path original_path = lake_root / *(*row_before)->file;
  const fs::path partition_dir = original_path.parent_path();

  // The orphan: a copy of the (still fully valid, un-rewritten) batch under
  // the filename gc()'s own numbering would use FIRST -- standing in for "a
  // prior gc() run wrote its rewritten file durably, then died before
  // publishing the catalog update or deleting the old file".
  const fs::path orphan_path = partition_dir / "batch-gc-000000.parquet";
  std::error_code copy_ec;
  ASSERT_TRUE(fs::copy_file(original_path, orphan_path, copy_ec)) << copy_ec.message();

  set_last_access_ts_raw(lake_root, old_key.hex(), 1000);
  set_last_access_ts_raw(lake_root, new_key.hex(), 5000);

  auto stats = gc(lake_root.string(), 3000);
  ASSERT_TRUE(stats.has_value()) << (stats ? "" : stats.error().to_string());
  EXPECT_EQ(stats->batches_rewritten, 1u);

  // The pre-existing orphan must survive UNTOUCHED -- gc() must have picked
  // the NEXT available name rather than colliding with (and silently
  // overwriting) it.
  EXPECT_TRUE(fs::exists(orphan_path)) << "a genuine crash orphan is not auto-swept by a later gc() "
                                          "run (documented, accepted gap) -- but it must not be "
                                          "clobbered either";
  const std::vector<std::string> orphan_keys = read_track_keys(orphan_path);
  EXPECT_EQ(std::count(orphan_keys.begin(), orphan_keys.end(), old_key.hex()), 3)
      << "the orphan's own content must be untouched by this run";

  auto new_row = catalog->probe(new_key);
  ASSERT_TRUE(new_row.has_value() && new_row->has_value());
  ASSERT_TRUE((*new_row)->file.has_value());
  EXPECT_NE(*(*new_row)->file, "tracks/underlier=SPY/family=strangle/batch-gc-000000.parquet")
      << "gc() must not have reused the orphan's name for its own real output";
  const fs::path real_new_path = lake_root / *(*new_row)->file;
  ASSERT_TRUE(fs::exists(real_new_path));
  const std::vector<std::string> real_keys = read_track_keys(real_new_path);
  EXPECT_EQ(std::count(real_keys.begin(), real_keys.end(), new_key.hex()), 3);

  EXPECT_EQ(raw_count(lake_root, "SELECT COUNT(*) FROM tracks WHERE file = 'tracks/underlier=SPY/"
                                 "family=strangle/batch-gc-000000.parquet';"),
            0)
      << "nothing in the catalog may ever point at the orphan -- it is inert, unreferenced disk";
}
