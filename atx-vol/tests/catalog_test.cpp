// Catalog -- SQLite catalog for the backtest lakehouse (Task D3,
// backtest-production-lakehouse sprint). atx/vol/catalog.hpp (Tier-B, E2
// promotion), src/catalog.cpp.
//
// Only built when ATX_VOL_LAKEHOUSE is ON (tests/CMakeLists.txt) -- Catalog's
// implementation does not exist in the OFF build, mirroring track_store_test.cpp.
//
// Covers the brief's Step 1 gate: (a) probe-miss -> register -> probe-hit
// round trip; (b) two REAL processes (this binary, spawned a second time --
// process_scratch_test.cpp's proven `_spawnl` shape, not a bespoke CLI flag)
// each inserting 1000 rows, running CONCURRENTLY (a background thread holds
// the spawn+wait blocking call so the parent's own 1000 inserts race it for
// real, not sequentially before/after), landing 2000 total rows with a clean
// `PRAGMA integrity_check`; plus WAL-mode assertion via PRAGMA readback,
// single-writer-fails-fast under real cross-connection contention (bounded,
// not an indefinite block), and the rest of the CRUD/validation surface.

#include "atx/vol/catalog.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <process.h> // _spawnl, _P_WAIT
#include <windows.h> // GetModuleFileNameA, GetEnvironmentVariableA, _putenv_s
#else
#include <cstdlib>
#endif

#include <gtest/gtest.h>

#include "atx/core/db/sqlite.hpp"

namespace atx::vol {
namespace {

namespace fs = std::filesystem;
namespace db = atx::core::db;

[[nodiscard]] fs::path fresh_lake_root(std::string_view tag) {
  static std::atomic<std::uint64_t> sequence{0};
  const fs::path dir = fs::temp_directory_path() /
                       ("atx_catalog_" + std::string(tag) + "_" +
                        std::to_string(sequence.fetch_add(1)));
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

[[nodiscard]] TrackKey make_indexed_key(std::uint8_t tag, std::uint64_t index) {
  TrackKey k;
  k.sha256[0] = tag;
  for (int i = 0; i < 8; ++i) {
    k.sha256[1 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(index >> (8 * i));
  }
  return k;
}

[[nodiscard]] TrackMeta make_meta() { return TrackMeta{"SPY", "strangle_hedged"}; }

[[nodiscard]] TrackRegistration make_registration(std::uint64_t index = 0) {
  TrackRegistration reg;
  reg.config_json = R"({"i":)" + std::to_string(index) + "}";
  reg.engine_id = "test-engine-rev1";
  reg.economics_rev = 1;
  reg.data_snapshot_id = "snapshot-0";
  reg.date_min = "2026-01-02";
  reg.date_max = "2026-01-30";
  return reg;
}

[[nodiscard]] std::int64_t raw_count(const fs::path &db_path, std::string_view sql) {
  auto conn = db::Database::open(db_path.string(), db::OpenMode::ReadOnly);
  EXPECT_TRUE(conn.has_value()) << (conn ? "" : conn.error().to_string());
  if (!conn) {
    return -1;
  }
  auto stmt = conn->prepare(sql);
  EXPECT_TRUE(stmt.has_value());
  if (!stmt) {
    return -1;
  }
  auto step = stmt->step();
  EXPECT_TRUE(step.has_value());
  if (!step || *step != db::Statement::Step::Row) {
    return -1;
  }
  return stmt->column_int(0);
}

// Task D6 test backdoor: `Catalog` deliberately exposes no way to set
// `last_access_ts` to anything but "now" (register_staging's own contract) --
// there is no production reason a caller should be able to forge it. Testing
// `retire_stale`'s age threshold deterministically (not via a real sleep,
// which would make the suite slow and flaky) needs SOME way to plant an old
// timestamp, so this reaches around the public API with a raw connection,
// exactly like `raw_count` above already does for read-only inspection.
void set_last_access_ts_raw(const fs::path &db_path, const std::string &track_key, std::int64_t ts) {
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

// Raw insert into reader_marks, for planting a mark whose pid is GUARANTEED
// dead without needing to spawn and wait on a real child process -- same
// "not a real PID on any host this test runs on" convention
// writer_lock_test.cpp's StaleLockWithDeadOwnerPidIsTakenOver uses.
constexpr std::int64_t kDefinitelyDeadPid = 999999999;

void insert_raw_reader_mark(const fs::path &db_path, std::string_view file, std::int64_t pid) {
  auto conn = db::Database::open(db_path.string(), db::OpenMode::ReadWrite);
  ASSERT_TRUE(conn.has_value()) << (conn ? "" : conn.error().to_string());
  auto stmt = conn->prepare("INSERT INTO reader_marks(file, pid, created_ts) VALUES (?1, ?2, 0);");
  ASSERT_TRUE(stmt.has_value());
  ASSERT_TRUE(stmt->bind(1, file).has_value());
  ASSERT_TRUE(stmt->bind(2, pid).has_value());
  auto step = stmt->step();
  ASSERT_TRUE(step.has_value()) << (step ? "" : step.error().to_string());
}

// ── Basic open / schema / WAL ───────────────────────────────────────────────

TEST(CatalogTest, OpenCreatesDbFile) {
  const fs::path root = fresh_lake_root("open");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value()) << (cat ? "" : cat.error().to_string());
  EXPECT_TRUE(fs::exists(root / std::string(kCatalogDbName)));
}

TEST(CatalogTest, ReopeningAnExistingCatalogIsIdempotentAndSeesPriorData) {
  const fs::path root = fresh_lake_root("reopen");
  {
    auto first = Catalog::open(root.string());
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->register_staging(make_indexed_key(0, 1), make_meta(), make_registration())
                    .has_value());
  }
  auto second = Catalog::open(root.string());
  ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());
  auto row = second->probe(make_indexed_key(0, 1));
  ASSERT_TRUE(row.has_value());
  ASSERT_TRUE(row->has_value()) << "re-opened catalog lost data written by the first handle";
}

TEST(CatalogTest, WalModeIsEnabled) {
  const fs::path root = fresh_lake_root("wal");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value()) << (cat ? "" : cat.error().to_string());

  // Independent connection: WAL is persisted IN the database file (unlike
  // most pragmas, it survives across connections), so this proves `open()`
  // actually landed the mode durably, not merely for its own live handle.
  auto conn = db::Database::open((root / std::string(kCatalogDbName)).string(), db::OpenMode::ReadWrite);
  ASSERT_TRUE(conn.has_value());
  auto stmt = conn->prepare("PRAGMA journal_mode;");
  ASSERT_TRUE(stmt.has_value());
  auto step = stmt->step();
  ASSERT_TRUE(step.has_value());
  ASSERT_EQ(*step, db::Statement::Step::Row);
  EXPECT_EQ(stmt->column_text(0), "wal");
}

// ── probe / register_staging round trip (brief Step 1a) ────────────────────

TEST(CatalogTest, ProbeMissThenRegisterThenProbeHit) {
  const fs::path root = fresh_lake_root("roundtrip");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const TrackKey key = make_indexed_key(0, 42);

  auto miss = cat->probe(key);
  ASSERT_TRUE(miss.has_value()) << (miss ? "" : miss.error().to_string());
  EXPECT_FALSE(miss->has_value()) << "probe found a row before anything was registered";

  const TrackMeta meta = make_meta();
  const TrackRegistration reg = make_registration(42);
  ASSERT_TRUE(cat->register_staging(key, meta, reg).has_value());

  auto hit = cat->probe(key);
  ASSERT_TRUE(hit.has_value()) << (hit ? "" : hit.error().to_string());
  ASSERT_TRUE(hit->has_value());
  const TrackRow &row = **hit;
  EXPECT_EQ(row.track_key, key.hex());
  EXPECT_EQ(row.underlier, meta.underlier);
  EXPECT_EQ(row.family, meta.family);
  EXPECT_EQ(row.config_json, reg.config_json);
  EXPECT_EQ(row.engine_id, reg.engine_id);
  EXPECT_EQ(row.economics_rev, reg.economics_rev);
  EXPECT_EQ(row.data_snapshot_id, reg.data_snapshot_id);
  EXPECT_EQ(row.date_min, reg.date_min);
  EXPECT_EQ(row.date_max, reg.date_max);
  EXPECT_EQ(row.status, TrackStatus::Staging);
  EXPECT_FALSE(row.file.has_value());
  EXPECT_FALSE(row.row_group.has_value());
  EXPECT_GT(row.created_ts, 0);
  EXPECT_EQ(row.created_ts, row.last_access_ts);
}

TEST(CatalogTest, RegisterStagingDuplicateKeyFailsAlreadyExists) {
  const fs::path root = fresh_lake_root("dup");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const TrackKey key = make_indexed_key(0, 7);
  ASSERT_TRUE(cat->register_staging(key, make_meta(), make_registration()).has_value());

  auto second = cat->register_staging(key, make_meta(), make_registration());
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code(), atx::core::ErrorCode::AlreadyExists);
}

TEST(CatalogTest, RegisterStagingRejectsEmptyUnderlierOrFamily) {
  const fs::path root = fresh_lake_root("badmeta");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());

  auto no_underlier =
      cat->register_staging(make_indexed_key(0, 1), TrackMeta{"", "strangle"}, make_registration());
  ASSERT_FALSE(no_underlier.has_value());
  EXPECT_EQ(no_underlier.error().code(), atx::core::ErrorCode::InvalidArgument);

  auto no_family =
      cat->register_staging(make_indexed_key(0, 2), TrackMeta{"SPY", ""}, make_registration());
  ASSERT_FALSE(no_family.has_value());
  EXPECT_EQ(no_family.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(CatalogTest, RegisterStagingRejectsDateMaxBeforeDateMin) {
  const fs::path root = fresh_lake_root("baddate");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());

  TrackRegistration reg = make_registration();
  reg.date_min = "2026-02-01";
  reg.date_max = "2026-01-01";
  auto result = cat->register_staging(make_indexed_key(0, 1), make_meta(), reg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ── economics-rev supersession (Task D5) ────────────────────────────────────
//
// A revision bump changes `engine_id` (track_key.hpp's `make_engine_id()`),
// which changes every `TrackKey` -- so the "new" generation's row is always a
// FRESH insert, never a collision with the old one. What `register_staging`
// additionally does now is retire the OLD generation's row(s) for the SAME
// logical variant: same `underlier`/`family`/`config_json`/
// `data_snapshot_id` (all of which stay byte-identical across a rev bump --
// none of the four encodes `engine_id`), but a STRICTLY LOWER
// `economics_rev`. Matched purely on the integer `economics_rev` field
// (never `created_ts`/wall-clock), so the outcome cannot depend on which
// generation happened to be registered "first" in real time -- see the I1-I8
// determinism note this task's controller flagged. Retiring is a STATUS
// LABEL only: the row is never deleted, so both generations stay
// `probe()`-able (and, on the Python side, `atxpy.tracks.catalog()`'s
// unfiltered `SELECT * FROM tracks` still returns it) -- "kept queryable" per
// the brief.
//
// A real `run_sweep` call can never itself construct this scenario within one
// build (`kBacktestEconomicsRev` is a compile-time constant -- see
// track_key.hpp), so these tests exercise the boundary directly at the
// `Catalog` layer with hand-crafted `TrackKey`/`TrackRegistration` values that
// mimic what TWO builds at two different revs would each independently
// produce for the identical variant (same config_json/data_snapshot_id, two
// different track_keys because `make_engine_id()` folded a different rev into
// each build's `engine_id`).

[[nodiscard]] TrackRegistration make_registration_at_rev(std::int64_t economics_rev,
                                                          const std::string &config_json,
                                                          const std::string &data_snapshot_id) {
  TrackRegistration reg;
  reg.config_json = config_json;
  reg.engine_id = "test-engine-rev" + std::to_string(economics_rev);
  reg.economics_rev = economics_rev;
  reg.data_snapshot_id = data_snapshot_id;
  reg.date_min = "2026-01-02";
  reg.date_max = "2026-01-30";
  return reg;
}

TEST(CatalogTest, RegisterStagingRetiresOlderGenerationRowForTheSameVariant) {
  const fs::path root = fresh_lake_root("supersede-basic");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());

  const TrackMeta meta = make_meta();
  const std::string config_json = R"({"template_id":"strangle","canonical_config_hex":"ab12"})";
  const std::string data_snapshot_id = "snapshot-fixed";

  // Generation 1 (rev 1) -- as if produced by today's build.
  const TrackKey gen1_key = make_indexed_key(1, 0);
  ASSERT_TRUE(
      cat->register_staging(gen1_key, meta, make_registration_at_rev(1, config_json, data_snapshot_id))
          .has_value());
  // The old generation gets compacted (a realistic timeline: it has been
  // sitting in the lakehouse a while by the time a rev bump ships) --
  // retiring must preserve file/row_group, not just flip status blind.
  ASSERT_TRUE(cat->mark_compacted(gen1_key, "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet",
                                  0)
                  .has_value());

  // Generation 2 (rev 2) -- as if produced by a rebuilt binary after
  // kBacktestEconomicsRev bumped. SAME config_json/data_snapshot_id/meta,
  // DIFFERENT track_key (different engine_id folded into the hash).
  const TrackKey gen2_key = make_indexed_key(2, 0);
  ASSERT_TRUE(
      cat->register_staging(gen2_key, meta, make_registration_at_rev(2, config_json, data_snapshot_id))
          .has_value());

  // Old generation: retired, but still fully queryable with its compacted
  // placement intact (retiring is a label, not a delete/rewrite).
  auto old_row = cat->probe(gen1_key);
  ASSERT_TRUE(old_row.has_value());
  ASSERT_TRUE(old_row->has_value()) << "a retired row must still be probe()-able";
  EXPECT_EQ((*old_row)->status, TrackStatus::Retired);
  ASSERT_TRUE((*old_row)->file.has_value());
  EXPECT_EQ(*(*old_row)->file, "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet");
  ASSERT_TRUE((*old_row)->row_group.has_value());
  EXPECT_EQ(*(*old_row)->row_group, 0);
  EXPECT_EQ((*old_row)->economics_rev, 1);

  // New generation: staged normally, untouched by its own retire step.
  auto new_row = cat->probe(gen2_key);
  ASSERT_TRUE(new_row.has_value());
  ASSERT_TRUE(new_row->has_value());
  EXPECT_EQ((*new_row)->status, TrackStatus::Staging);
  EXPECT_EQ((*new_row)->economics_rev, 2);
}

TEST(CatalogTest, RegisterStagingNeverRetiresRowsAtTheSameEconomicsRev) {
  // "rev N vs N": two DIFFERENT track_keys can share config_json/
  // data_snapshot_id/meta at the identical economics_rev only via an
  // engine_id difference that is NOT an economics change (e.g. a
  // RunArchive-schema-only rebuild) -- kBacktestEconomicsRev unchanged.
  // Supersession must not fire: retiring is specifically about an economics
  // generation change, not incidental same-rev build churn.
  const fs::path root = fresh_lake_root("supersede-same-rev");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());

  const TrackMeta meta = make_meta();
  const std::string config_json = R"({"template_id":"strangle","canonical_config_hex":"cd34"})";
  const std::string data_snapshot_id = "snapshot-fixed-2";

  const TrackKey key_a = make_indexed_key(3, 0);
  ASSERT_TRUE(
      cat->register_staging(key_a, meta, make_registration_at_rev(5, config_json, data_snapshot_id))
          .has_value());
  const TrackKey key_b = make_indexed_key(4, 0);
  ASSERT_TRUE(
      cat->register_staging(key_b, meta, make_registration_at_rev(5, config_json, data_snapshot_id))
          .has_value());

  auto row_a = cat->probe(key_a);
  ASSERT_TRUE(row_a.has_value());
  ASSERT_TRUE(row_a->has_value());
  EXPECT_EQ((*row_a)->status, TrackStatus::Staging)
      << "economics_rev < registration.economics_rev is STRICT -- an equal rev must never retire";

  auto row_b = cat->probe(key_b);
  ASSERT_TRUE(row_b.has_value());
  ASSERT_TRUE(row_b->has_value());
  EXPECT_EQ((*row_b)->status, TrackStatus::Staging);
}

TEST(CatalogTest, RegisterStagingNeverRetroactivelyRetiresANewerGeneration) {
  // "rev N vs N-1", registered out of rev ORDER: a rev-1 registration must
  // never retire an already-registered rev-2 row for the same variant --
  // supersession compares economics_rev NUMERICALLY, never by which row
  // landed first (no wall-clock/created_ts ordering).
  const fs::path root = fresh_lake_root("supersede-out-of-order");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());

  const TrackMeta meta = make_meta();
  const std::string config_json = R"({"template_id":"strangle","canonical_config_hex":"ef56"})";
  const std::string data_snapshot_id = "snapshot-fixed-3";

  const TrackKey newer_key = make_indexed_key(5, 0);
  ASSERT_TRUE(
      cat->register_staging(newer_key, meta, make_registration_at_rev(2, config_json, data_snapshot_id))
          .has_value());
  const TrackKey older_key = make_indexed_key(6, 0);
  ASSERT_TRUE(
      cat->register_staging(older_key, meta, make_registration_at_rev(1, config_json, data_snapshot_id))
          .has_value());

  auto newer_row = cat->probe(newer_key);
  ASSERT_TRUE(newer_row.has_value());
  ASSERT_TRUE(newer_row->has_value());
  EXPECT_EQ((*newer_row)->status, TrackStatus::Staging)
      << "a later-registered OLDER rev must not retroactively retire an already-registered newer one";
}

TEST(CatalogTest, RegisterStagingSupersessionIsScopedToTheSameUnderlierFamilyAndSnapshot) {
  // config_json alone matching is not enough: underlier/family/
  // data_snapshot_id must ALSO match, or an unrelated track (different
  // hive placement, or the same economics replayed over different market
  // data) would be wrongly retired.
  const fs::path root = fresh_lake_root("supersede-scoped");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());

  const std::string config_json = R"({"template_id":"strangle","canonical_config_hex":"9988"})";

  const TrackKey diff_underlier_key = make_indexed_key(7, 0);
  ASSERT_TRUE(cat->register_staging(diff_underlier_key, TrackMeta{"SPY", "strangle_hedged"},
                                    make_registration_at_rev(1, config_json, "snap-x"))
                  .has_value());
  const TrackKey diff_snapshot_key = make_indexed_key(7, 1);
  ASSERT_TRUE(cat->register_staging(diff_snapshot_key, TrackMeta{"QQQ", "strangle_hedged"},
                                    make_registration_at_rev(1, config_json, "snap-y"))
                  .has_value());

  // A rev-2 registration for QQQ/snap-y (matching diff_snapshot_key exactly)
  // must retire ONLY diff_snapshot_key, never the SPY/snap-x row above --
  // same config_json, but a different underlier AND a different
  // data_snapshot_id.
  const TrackKey superseder_key = make_indexed_key(7, 2);
  ASSERT_TRUE(cat->register_staging(superseder_key, TrackMeta{"QQQ", "strangle_hedged"},
                                    make_registration_at_rev(2, config_json, "snap-y"))
                  .has_value());

  auto unrelated_row = cat->probe(diff_underlier_key);
  ASSERT_TRUE(unrelated_row.has_value());
  ASSERT_TRUE(unrelated_row->has_value());
  EXPECT_EQ((*unrelated_row)->status, TrackStatus::Staging)
      << "a different underlier's same-config row must never be retired";

  auto superseded_row = cat->probe(diff_snapshot_key);
  ASSERT_TRUE(superseded_row.has_value());
  ASSERT_TRUE(superseded_row->has_value());
  EXPECT_EQ((*superseded_row)->status, TrackStatus::Retired);
}

// ── list_by_status (Task D5 fix-round: track_compact_reconcile's own probe) ─

TEST(CatalogTest, ListByStatusReturnsOnlyMatchingRowsOrderedByTrackKey) {
  const fs::path root = fresh_lake_root("list-by-status");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());

  // Insertion order deliberately NOT track_key order, so a passing
  // "ordered by track_key" assertion actually exercises the SQL's own
  // ORDER BY rather than pandas-style insertion-order luck.
  const TrackKey key_b = make_indexed_key(9, 2);
  const TrackKey key_a = make_indexed_key(9, 1);
  const TrackKey key_c = make_indexed_key(9, 3);
  ASSERT_TRUE(cat->register_staging(key_b, make_meta(), make_registration(2)).has_value());
  ASSERT_TRUE(cat->register_staging(key_a, make_meta(), make_registration(1)).has_value());
  ASSERT_TRUE(cat->register_staging(key_c, make_meta(), make_registration(3)).has_value());
  ASSERT_TRUE(cat->mark_compacted(key_c, "batch.parquet", 0).has_value());

  auto staging_rows = cat->list_by_status(TrackStatus::Staging);
  ASSERT_TRUE(staging_rows.has_value()) << (staging_rows ? "" : staging_rows.error().to_string());
  ASSERT_EQ(staging_rows->size(), 2u) << "key_c is compacted, not staging";
  EXPECT_LT((*staging_rows)[0].track_key, (*staging_rows)[1].track_key) << "ORDER BY track_key";
  for (const TrackRow &row : *staging_rows) {
    EXPECT_EQ(row.status, TrackStatus::Staging);
  }

  auto compacted_rows = cat->list_by_status(TrackStatus::Compacted);
  ASSERT_TRUE(compacted_rows.has_value());
  ASSERT_EQ(compacted_rows->size(), 1u);
  EXPECT_EQ((*compacted_rows)[0].track_key, key_c.hex());

  auto retired_rows = cat->list_by_status(TrackStatus::Retired);
  ASSERT_TRUE(retired_rows.has_value());
  EXPECT_TRUE(retired_rows->empty());
}

// ── mark_compacted ───────────────────────────────────────────────────────

TEST(CatalogTest, MarkCompactedTransitionsStagingToCompacted) {
  const fs::path root = fresh_lake_root("compact");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const TrackKey key = make_indexed_key(0, 5);
  ASSERT_TRUE(cat->register_staging(key, make_meta(), make_registration()).has_value());

  ASSERT_TRUE(cat->mark_compacted(key, "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet", 0)
                  .has_value());

  auto row = cat->probe(key);
  ASSERT_TRUE(row.has_value());
  ASSERT_TRUE(row->has_value());
  EXPECT_EQ((*row)->status, TrackStatus::Compacted);
  ASSERT_TRUE((*row)->file.has_value());
  EXPECT_EQ(*(*row)->file, "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet");
  ASSERT_TRUE((*row)->row_group.has_value());
  EXPECT_EQ(*(*row)->row_group, 0);
}

TEST(CatalogTest, MarkCompactedOnUnknownTrackFailsNotFound) {
  const fs::path root = fresh_lake_root("compact-missing");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  auto result = cat->mark_compacted(make_indexed_key(0, 99), "batch.parquet", 0);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::NotFound);
}

TEST(CatalogTest, MarkCompactedTwiceFailsInvalidArgument) {
  const fs::path root = fresh_lake_root("compact-twice");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const TrackKey key = make_indexed_key(0, 3);
  ASSERT_TRUE(cat->register_staging(key, make_meta(), make_registration()).has_value());
  ASSERT_TRUE(cat->mark_compacted(key, "batch-a.parquet", 0).has_value());

  auto again = cat->mark_compacted(key, "batch-b.parquet", 1);
  ASSERT_FALSE(again.has_value())
      << "a track already compacted must not silently accept a second compaction";
  EXPECT_EQ(again.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ── Task D6: retention/GC support ───────────────────────────────────────────
//
// gc()'s own orchestration (Arrow-touching batch rewrite/delete) is tested in
// track_gc_test.cpp -- these are the pure-Catalog pieces gc() is built on:
// retire_stale (the last_access_ts-driven status transition), rows_by_file
// (a batch's full membership), the reader_marks advisory-mark table (the
// brief's "reader takes a shared advisory mark in SQLite, GC skips marked
// batches" replacement for the superseded readers_epoch mechanism), and
// apply_gc_rewrite (the one transaction that publishes a rewrite/delete).

TEST(CatalogTest, RetireStaleRetiresOnlyCompactedRowsPastTheThreshold) {
  const fs::path root = fresh_lake_root("retire-stale");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const fs::path db_path = root / std::string(kCatalogDbName);

  const TrackKey old_key = make_indexed_key(0, 1);
  const TrackKey new_key = make_indexed_key(0, 2);
  const TrackKey staging_key = make_indexed_key(0, 3);
  ASSERT_TRUE(cat->register_staging(old_key, make_meta(), make_registration(1)).has_value());
  ASSERT_TRUE(cat->mark_compacted(old_key, "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet", 0)
                  .has_value());
  ASSERT_TRUE(cat->register_staging(new_key, make_meta(), make_registration(2)).has_value());
  ASSERT_TRUE(cat->mark_compacted(new_key, "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet", 0)
                  .has_value());
  ASSERT_TRUE(cat->register_staging(staging_key, make_meta(), make_registration(3)).has_value());

  set_last_access_ts_raw(db_path, old_key.hex(), 1000);
  set_last_access_ts_raw(db_path, new_key.hex(), 5000);

  auto retired = cat->retire_stale(3000);
  ASSERT_TRUE(retired.has_value()) << (retired ? "" : retired.error().to_string());
  EXPECT_EQ(*retired, 1) << "only the old COMPACTED row is past the threshold";

  auto old_row = cat->probe(old_key);
  ASSERT_TRUE(old_row.has_value());
  ASSERT_TRUE(old_row->has_value());
  EXPECT_EQ((*old_row)->status, TrackStatus::Retired);
  ASSERT_TRUE((*old_row)->file.has_value()) << "retire_stale is a status label only -- never touches file/row_group";
  EXPECT_EQ(*(*old_row)->file, "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet");

  auto new_row = cat->probe(new_key);
  ASSERT_TRUE(new_row.has_value());
  ASSERT_TRUE(new_row->has_value());
  EXPECT_EQ((*new_row)->status, TrackStatus::Compacted) << "newer than the threshold must survive";

  auto staging_row = cat->probe(staging_key);
  ASSERT_TRUE(staging_row.has_value());
  ASSERT_TRUE(staging_row->has_value());
  EXPECT_EQ((*staging_row)->status, TrackStatus::Staging)
      << "retire_stale only ever targets Compacted rows, never Staging";

  // Idempotent: nothing left to retire on a second call at the same threshold.
  auto second = cat->retire_stale(3000);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*second, 0);
}

TEST(CatalogTest, RowsByFileReturnsEveryRowPointingAtThatFileOrderedByKey) {
  const fs::path root = fresh_lake_root("rows-by-file");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());

  const TrackKey key_b = make_indexed_key(1, 2);
  const TrackKey key_a = make_indexed_key(1, 1);
  const TrackKey other_file_key = make_indexed_key(1, 3);
  ASSERT_TRUE(cat->register_staging(key_b, make_meta(), make_registration(2)).has_value());
  ASSERT_TRUE(cat->register_staging(key_a, make_meta(), make_registration(1)).has_value());
  ASSERT_TRUE(cat->register_staging(other_file_key, make_meta(), make_registration(3)).has_value());
  ASSERT_TRUE(cat->mark_compacted(key_b, "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet", 0)
                  .has_value());
  ASSERT_TRUE(cat->mark_compacted(key_a, "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet", 0)
                  .has_value());
  ASSERT_TRUE(
      cat->mark_compacted(other_file_key, "tracks/underlier=SPY/family=strangle_hedged/batch-000001.parquet", 0)
          .has_value());

  auto rows = cat->rows_by_file("tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet");
  ASSERT_TRUE(rows.has_value()) << (rows ? "" : rows.error().to_string());
  ASSERT_EQ(rows->size(), 2u);
  EXPECT_EQ((*rows)[0].track_key, key_a.hex()) << "ORDER BY track_key";
  EXPECT_EQ((*rows)[1].track_key, key_b.hex());

  auto empty = cat->rows_by_file("tracks/underlier=SPY/family=strangle_hedged/batch-999999.parquet");
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
}

TEST(CatalogTest, MarkReaderThenHasLiveReaderMarkIsTrue) {
  const fs::path root = fresh_lake_root("mark-live");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());

  auto before = cat->has_live_reader_mark("tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet");
  ASSERT_TRUE(before.has_value());
  EXPECT_FALSE(*before) << "no mark registered yet";

  auto mark_id = cat->mark_reader("tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet");
  ASSERT_TRUE(mark_id.has_value()) << (mark_id ? "" : mark_id.error().to_string());
  EXPECT_GT(*mark_id, 0);

  auto after = cat->has_live_reader_mark("tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet");
  ASSERT_TRUE(after.has_value());
  EXPECT_TRUE(*after) << "mark_reader uses the CALLING process's own (live) pid";

  auto other_file = cat->has_live_reader_mark("tracks/underlier=SPY/family=strangle_hedged/batch-000001.parquet");
  ASSERT_TRUE(other_file.has_value());
  EXPECT_FALSE(*other_file) << "a mark on one file must not protect a different file";
}

TEST(CatalogTest, ReleaseReaderMarkClearsLivenessAndIsIdempotent) {
  const fs::path root = fresh_lake_root("mark-release");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const std::string file = "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet";

  auto mark_id = cat->mark_reader(file);
  ASSERT_TRUE(mark_id.has_value());
  ASSERT_TRUE(cat->has_live_reader_mark(file).value_or(false));

  ASSERT_TRUE(cat->release_reader_mark(*mark_id).has_value());
  auto after = cat->has_live_reader_mark(file);
  ASSERT_TRUE(after.has_value());
  EXPECT_FALSE(*after);

  // Idempotent: releasing an already-released (or never-existent) id is not
  // an error -- mirrors WriterLock::release()'s own idempotence contract.
  EXPECT_TRUE(cat->release_reader_mark(*mark_id).has_value());
  EXPECT_TRUE(cat->release_reader_mark(123456789).has_value());
}

TEST(CatalogTest, HasLiveReaderMarkIgnoresAndCleansUpADeadPidMark) {
  const fs::path root = fresh_lake_root("mark-stale");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const fs::path db_path = root / std::string(kCatalogDbName);
  const std::string file = "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet";

  // Simulates a reader that registered a mark and then crashed without
  // releasing it -- reachable in the real system, but not reproducible
  // deterministically by actually killing a process, so this plants the
  // post-crash STATE directly, same discipline track_compact_reconcile_test
  // .cpp uses for track_compact's own crash window.
  insert_raw_reader_mark(db_path, file, kDefinitelyDeadPid);
  EXPECT_EQ(raw_count(db_path, "SELECT COUNT(*) FROM reader_marks;"), 1);

  auto live = cat->has_live_reader_mark(file);
  ASSERT_TRUE(live.has_value()) << (live ? "" : live.error().to_string());
  EXPECT_FALSE(*live) << "a dead-pid mark must not be treated as live";

  EXPECT_EQ(raw_count(db_path, "SELECT COUNT(*) FROM reader_marks;"), 0)
      << "a confirmed-dead mark is opportunistically cleaned up while scanning, mirroring "
         "WriterLock's own stale-owner takeover";
}

TEST(CatalogTest, ApplyGcRewriteWholeBatchDeleteClearsEveryRetiredRow) {
  const fs::path root = fresh_lake_root("gc-rewrite-delete");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const fs::path db_path = root / std::string(kCatalogDbName);
  const std::string old_file = "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet";

  const TrackKey key_a = make_indexed_key(2, 1);
  const TrackKey key_b = make_indexed_key(2, 2);
  ASSERT_TRUE(cat->register_staging(key_a, make_meta(), make_registration(1)).has_value());
  ASSERT_TRUE(cat->mark_compacted(key_a, old_file, 0).has_value());
  ASSERT_TRUE(cat->register_staging(key_b, make_meta(), make_registration(2)).has_value());
  ASSERT_TRUE(cat->mark_compacted(key_b, old_file, 0).has_value());

  // Both tracks in this batch are now old enough to retire -- the "whole
  // batch reclaimed" scenario (kept_keys empty), a different apply_gc_rewrite
  // code path than the survivors test above (new_file non-empty).
  set_last_access_ts_raw(db_path, key_a.hex(), 1000);
  set_last_access_ts_raw(db_path, key_b.hex(), 1000);
  ASSERT_TRUE(cat->retire_stale(2000).has_value());

  const std::vector<std::string> retired_keys{key_a.hex(), key_b.hex()};
  ASSERT_TRUE(cat->apply_gc_rewrite(old_file, "", retired_keys, {}).has_value());

  for (const TrackKey &key : {key_a, key_b}) {
    auto row = cat->probe(key);
    ASSERT_TRUE(row.has_value());
    ASSERT_TRUE(row->has_value());
    EXPECT_EQ((*row)->status, TrackStatus::Retired);
    EXPECT_FALSE((*row)->file.has_value())
        << "physically-reclaimed retired data must clear file, not keep pointing at deleted bytes";
    EXPECT_FALSE((*row)->row_group.has_value());
  }
}

TEST(CatalogTest, ApplyGcRewriteRepointsSurvivorsAtTheNewFile) {
  const fs::path root = fresh_lake_root("gc-rewrite-survivors");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const std::string old_file = "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet";
  const std::string new_file = "tracks/underlier=SPY/family=strangle_hedged/batch-gc-000000.parquet";

  const TrackKey survivor = make_indexed_key(3, 1);
  const TrackKey reclaimed = make_indexed_key(3, 2);
  ASSERT_TRUE(cat->register_staging(survivor, make_meta(), make_registration(1)).has_value());
  ASSERT_TRUE(cat->mark_compacted(survivor, old_file, 0).has_value());
  ASSERT_TRUE(cat->register_staging(reclaimed, make_meta(), make_registration(2)).has_value());
  ASSERT_TRUE(cat->mark_compacted(reclaimed, old_file, 0).has_value());

  // Retire ONLY `reclaimed` directly (bypassing retire_stale's age semantics,
  // which are covered by their own test) via a fresh registration at a
  // strictly higher economics_rev for the SAME variant -- D5's supersession
  // path, the other real way a row becomes Retired.
  TrackRegistration superseder = make_registration(2);
  superseder.economics_rev = 2;
  const TrackKey superseder_key = make_indexed_key(3, 3);
  ASSERT_TRUE(cat->register_staging(superseder_key, make_meta(), superseder).has_value());
  auto sanity_row = cat->probe(reclaimed);
  ASSERT_TRUE(sanity_row.has_value());
  ASSERT_TRUE(sanity_row->has_value());
  ASSERT_EQ((*sanity_row)->status, TrackStatus::Retired) << "sanity: supersession fired";

  const std::vector<std::string> retired_keys{reclaimed.hex()};
  const std::vector<std::string> kept_keys{survivor.hex()};
  ASSERT_TRUE(cat->apply_gc_rewrite(old_file, new_file, retired_keys, kept_keys).has_value());

  auto survivor_row = cat->probe(survivor);
  ASSERT_TRUE(survivor_row.has_value());
  ASSERT_TRUE(survivor_row->has_value());
  EXPECT_EQ((*survivor_row)->status, TrackStatus::Compacted);
  ASSERT_TRUE((*survivor_row)->file.has_value());
  EXPECT_EQ(*(*survivor_row)->file, new_file);
  ASSERT_TRUE((*survivor_row)->row_group.has_value());
  EXPECT_EQ(*(*survivor_row)->row_group, 0);

  auto reclaimed_row = cat->probe(reclaimed);
  ASSERT_TRUE(reclaimed_row.has_value());
  ASSERT_TRUE(reclaimed_row->has_value());
  EXPECT_EQ((*reclaimed_row)->status, TrackStatus::Retired);
  EXPECT_FALSE((*reclaimed_row)->file.has_value());
}

TEST(CatalogTest, ApplyGcRewriteRejectsSurvivorsWithoutANewFile) {
  const fs::path root = fresh_lake_root("gc-rewrite-guard");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());

  const std::vector<std::string> retired_keys{};
  const std::vector<std::string> kept_keys{"deadbeef"};
  auto result = cat->apply_gc_rewrite("tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet", "",
                                      retired_keys, kept_keys);
  ASSERT_FALSE(result.has_value())
      << "a batch delete (empty new_file) must never claim survivors -- that would silently orphan them";
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// Review finding (Important, fix-round 1): a per-key UPDATE matching ZERO
// rows is not a SQLite error, so a caller-supplied kept_keys/retired_keys
// entry that no longer matches its expected (status, file) -- because a
// CONCURRENT writer changed it between the caller's own rows_by_file()
// snapshot and this call -- used to silently no-op and still commit Ok().
// track_gc.cpp then physically deleted old_file unconditionally, leaving
// the stale row pointing at bytes that no longer exist. Both loops must now
// fail closed (Err(Internal), whole transaction rolled back) the instant
// any single UPDATE affects anything other than exactly one row.

TEST(CatalogTest, ApplyGcRewriteFailsClosedWhenAKeptRowWasConcurrentlyRetired) {
  const fs::path root = fresh_lake_root("gc-rewrite-race");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const std::string old_file = "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet";
  const std::string new_file = "tracks/underlier=SPY/family=strangle_hedged/batch-gc-000000.parquet";

  const TrackKey kept_key = make_indexed_key(4, 1);
  const TrackKey other_retired_key = make_indexed_key(4, 2);
  const std::string config_json = R"({"template_id":"strangle","canonical_config_hex":"race"})";
  const std::string data_snapshot_id = "snapshot-race";
  ASSERT_TRUE(cat->register_staging(kept_key, make_meta(), make_registration_at_rev(1, config_json, data_snapshot_id))
                  .has_value());
  ASSERT_TRUE(cat->mark_compacted(kept_key, old_file, 0).has_value());
  ASSERT_TRUE(cat->register_staging(other_retired_key, make_meta(), make_registration(2)).has_value());
  ASSERT_TRUE(cat->mark_compacted(other_retired_key, old_file, 0).has_value());

  // Simulates the finding's race directly: GC snapshotted membership
  // (kept_key still 'compacted') and started rewriting old_file's Parquet
  // bytes, but BEFORE apply_gc_rewrite runs, a concurrent register_staging
  // supersedes kept_key via D5 economics-rev retirement -- a real, ordinary
  // event (same underlier/family/config_json/data_snapshot_id, higher
  // economics_rev), not a contrived one.
  const TrackKey superseder_key = make_indexed_key(4, 3);
  ASSERT_TRUE(cat->register_staging(superseder_key, make_meta(),
                                    make_registration_at_rev(2, config_json, data_snapshot_id))
                  .has_value());
  auto raced_row = cat->probe(kept_key);
  ASSERT_TRUE(raced_row.has_value() && raced_row->has_value());
  ASSERT_EQ((*raced_row)->status, TrackStatus::Retired) << "sanity: the race actually fired";
  ASSERT_TRUE((*raced_row)->file.has_value()) << "supersession never touches file/row_group by itself";
  EXPECT_EQ(*(*raced_row)->file, old_file);

  const std::vector<std::string> retired_keys{other_retired_key.hex()};
  const std::vector<std::string> kept_keys{kept_key.hex()}; // stale -- no longer actually 'compacted'
  auto result = cat->apply_gc_rewrite(old_file, new_file, retired_keys, kept_keys);
  ASSERT_FALSE(result.has_value())
      << "a stale kept_keys entry must fail closed, never silently no-op and commit";
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::Internal);

  // Fails CLOSED: the whole transaction rolled back, including the
  // retired_keys half that would have succeeded on its own.
  auto other_row = cat->probe(other_retired_key);
  ASSERT_TRUE(other_row.has_value() && other_row->has_value());
  EXPECT_TRUE((*other_row)->file.has_value())
      << "the transaction must roll back atomically -- no partial apply, even for the half that matched";
  auto kept_row_after = cat->probe(kept_key);
  ASSERT_TRUE(kept_row_after.has_value() && kept_row_after->has_value());
  ASSERT_TRUE((*kept_row_after)->file.has_value());
  EXPECT_EQ(*(*kept_row_after)->file, old_file) << "unchanged -- rolled back, not repointed";
}

TEST(CatalogTest, ApplyGcRewriteFailsClosedWhenARetiredKeyDoesNotActuallyMatch) {
  const fs::path root = fresh_lake_root("gc-rewrite-badretired");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const std::string old_file = "tracks/underlier=SPY/family=strangle_hedged/batch-000000.parquet";

  const TrackKey still_compacted_key = make_indexed_key(5, 1);
  ASSERT_TRUE(cat->register_staging(still_compacted_key, make_meta(), make_registration(1)).has_value());
  ASSERT_TRUE(cat->mark_compacted(still_compacted_key, old_file, 0).has_value());

  // still_compacted_key was never retired -- claiming it as a retired_keys
  // entry (a caller bug, or a snapshot that went stale) must not silently
  // clear its file pointer: the WHERE clause's status='retired' guard
  // rejects this UPDATE (0 rows changed), and that must surface as a hard
  // failure, not a quiet no-op.
  const std::vector<std::string> retired_keys{still_compacted_key.hex()};
  auto result = cat->apply_gc_rewrite(old_file, "", retired_keys, {});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::Internal);

  auto row = cat->probe(still_compacted_key);
  ASSERT_TRUE(row.has_value() && row->has_value());
  EXPECT_EQ((*row)->status, TrackStatus::Compacted) << "unaffected -- rolled back";
  ASSERT_TRUE((*row)->file.has_value()) << "file pointer must survive a rejected/rolled-back call";
  EXPECT_EQ(*(*row)->file, old_file);
}

// ── record_trial / trial_stats (feeds B4's DSR) ─────────────────────────────

TEST(CatalogTest, RecordTrialAgainstUnknownTrackFails) {
  const fs::path root = fresh_lake_root("trial-missing");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  auto result = cat->record_trial(make_indexed_key(0, 1), "sweep-1", 1.0);
  ASSERT_FALSE(result.has_value())
      << "record_trial must not silently accept a track_key with no tracks row";
  // The FK violation surfaces as atx::core::db's SQLITE_CONSTRAINT mapping,
  // which is AlreadyExists (not NotFound) -- see catalog.hpp's doc comment
  // on record_trial for why that is the code a caller actually gets here.
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::AlreadyExists);
}

TEST(CatalogTest, TrialStatsForUnknownSweepIsZero) {
  const fs::path root = fresh_lake_root("trial-empty-sweep");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  auto stats = cat->trial_stats("no-such-sweep");
  ASSERT_TRUE(stats.has_value()) << (stats ? "" : stats.error().to_string());
  EXPECT_EQ(stats->n_trials, 0u);
  EXPECT_DOUBLE_EQ(stats->sr_variance, 0.0);
}

TEST(CatalogTest, RecordTrialAndTrialStatsComputesSampleVariance) {
  const fs::path root = fresh_lake_root("trial-variance");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const TrackKey key = make_indexed_key(0, 1);
  ASSERT_TRUE(cat->register_staging(key, make_meta(), make_registration()).has_value());

  // Sample (Bessel-corrected) variance of {1.0, 2.0, 3.0}: mean 2.0,
  // sum((x-mean)^2) = 1 + 0 + 1 = 2, / (N-1=2) = 1.0.
  for (const double sharpe : {1.0, 2.0, 3.0}) {
    auto id = cat->record_trial(key, "sweep-var", sharpe);
    ASSERT_TRUE(id.has_value()) << (id ? "" : id.error().to_string());
    EXPECT_GT(*id, 0);
  }

  auto stats = cat->trial_stats("sweep-var");
  ASSERT_TRUE(stats.has_value()) << (stats ? "" : stats.error().to_string());
  EXPECT_EQ(stats->n_trials, 3u);
  EXPECT_NEAR(stats->sr_variance, 1.0, 1e-12);
}

TEST(CatalogTest, RecordTrialIdsAreDistinctAndIncreasing) {
  const fs::path root = fresh_lake_root("trial-ids");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const TrackKey key = make_indexed_key(0, 1);
  ASSERT_TRUE(cat->register_staging(key, make_meta(), make_registration()).has_value());

  auto id1 = cat->record_trial(key, "sweep-ids", 0.5);
  auto id2 = cat->record_trial(key, "sweep-ids", 0.6);
  ASSERT_TRUE(id1.has_value());
  ASSERT_TRUE(id2.has_value());
  EXPECT_NE(*id1, *id2);
}

TEST(CatalogTest, TrialStatsCountsNullSharpeTowardNButExcludesFromVariance) {
  const fs::path root = fresh_lake_root("trial-null");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const TrackKey key = make_indexed_key(0, 1);
  ASSERT_TRUE(cat->register_staging(key, make_meta(), make_registration()).has_value());

  for (const double sharpe : {1.0, 2.0, 3.0}) {
    ASSERT_TRUE(cat->record_trial(key, "sweep-null", sharpe).has_value());
  }
  // A trial that did not produce a usable Sharpe -- still an attempted
  // variant, still counted in N, but excluded from the variance.
  ASSERT_TRUE(cat->record_trial(key, "sweep-null", std::nullopt).has_value());

  auto stats = cat->trial_stats("sweep-null");
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->n_trials, 4u) << "N must count EVERY attempted trial, not just usable ones";
  EXPECT_NEAR(stats->sr_variance, 1.0, 1e-12);
}

TEST(CatalogTest, TrialStatsSingleTrialVarianceIsZeroNotNan) {
  const fs::path root = fresh_lake_root("trial-single");
  auto cat = Catalog::open(root.string());
  ASSERT_TRUE(cat.has_value());
  const TrackKey key = make_indexed_key(0, 1);
  ASSERT_TRUE(cat->register_staging(key, make_meta(), make_registration()).has_value());
  ASSERT_TRUE(cat->record_trial(key, "sweep-one", 1.5).has_value());

  auto stats = cat->trial_stats("sweep-one");
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->n_trials, 1u);
  EXPECT_FALSE(std::isnan(stats->sr_variance));
  EXPECT_DOUBLE_EQ(stats->sr_variance, 0.0);
}

// ── single-writer enforcement: bounded fail-fast under real contention ─────

TEST(CatalogTest, ConcurrentWriterBeyondBusyTimeoutFailsFastNotIndefinitely) {
  const fs::path root = fresh_lake_root("busy");
  {
    auto pre = Catalog::open(root.string()); // schema created + committed, then closed
    ASSERT_TRUE(pre.has_value());
  }

  // A second, independent connection holds the SQLite write lock open via an
  // explicit IMMEDIATE transaction it never commits until after this test's
  // assertions -- a real cross-connection writer/writer collision, not a
  // same-handle reentrancy check.
  auto blocker = db::Database::open((root / std::string(kCatalogDbName)).string(), db::OpenMode::ReadWrite);
  ASSERT_TRUE(blocker.has_value());
  auto txn = db::Transaction::begin_immediate(*blocker);
  ASSERT_TRUE(txn.has_value()) << (txn ? "" : txn.error().to_string());

  // Short busy_timeout override so this test does not wait out the
  // production 5000ms default.
  auto second = Catalog::open(root.string(), std::chrono::milliseconds{150});
  ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());

  const auto started = std::chrono::steady_clock::now();
  auto result = second->register_staging(make_indexed_key(9, 1), make_meta(), make_registration());
  const auto elapsed = std::chrono::steady_clock::now() - started;

  ASSERT_FALSE(result.has_value())
      << "a write must fail while another connection holds the SQLite write lock open";
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::Unavailable);
  EXPECT_LT(elapsed, std::chrono::milliseconds{2000})
      << "busy_timeout did not bound the wait -- this looks like an indefinite block";

  ASSERT_TRUE(txn->commit().has_value());
  // Now that the blocker released, the SAME handle's next write succeeds.
  EXPECT_TRUE(second->register_staging(make_indexed_key(9, 2), make_meta(), make_registration())
                  .has_value());
}

// ── brief Step 1(b): two REAL processes, 1000 rows each, zero corruption ──

constexpr const char *kStressRootEnv = "ATX_VOL_CATALOG_STRESS_ROOT";

// Empty when unset. GetEnvironmentVariableA (not std::getenv, which this
// toolchain flags deprecated under /WX) -- same shape as
// process_scratch_test.cpp's probe_out().
[[nodiscard]] std::string stress_root_env() {
#ifdef _WIN32
  char buf[4096];
  const DWORD n = ::GetEnvironmentVariableA(kStressRootEnv, buf, static_cast<DWORD>(sizeof buf));
  if (n == 0 || n >= sizeof buf) {
    return {};
  }
  return std::string(buf, static_cast<std::size_t>(n));
#else
  const char *v = std::getenv(kStressRootEnv);
  return v == nullptr ? std::string{} : std::string{v};
#endif
}

[[nodiscard]] fs::path this_image() {
#ifdef _WIN32
  char buf[4096];
  const DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof buf));
  return fs::path(std::string(buf, static_cast<std::size_t>(n)));
#else
  std::error_code ec;
  return fs::read_symlink("/proc/self/exe", ec);
#endif
}

// Launches this same test binary as a second process running only `filter`,
// and BLOCKS until it exits -- process_scratch_test.cpp's proven
// `run_second_process` shape (a bespoke "--catalog-writer-stress" CLI flag
// would need its own argv interception ahead of GTest's own main(); this
// reuses the established, already-working mechanism instead). Called from a
// background std::thread (below) so the PARENT's own 1000 inserts run
// concurrently with the CHILD's, not sequentially before/after.
[[nodiscard]] int run_second_process(const std::string &filter) {
  const std::string exe = this_image().string();
  const std::string arg_filter = "--gtest_filter=" + filter;
#ifdef _WIN32
  const std::string quoted_argv0 = "\"" + exe + "\"";
  const std::intptr_t rc = ::_spawnl(_P_WAIT, exe.c_str(), quoted_argv0.c_str(),
                                     arg_filter.c_str(), "--gtest_brief=1", nullptr);
  return static_cast<int>(rc);
#else
  const std::string cmd = "\"" + exe + "\" " + arg_filter + " --gtest_brief=1";
  return std::system(cmd.c_str());
#endif
}

// RAII safety net for the driver test below. `ASSERT_*` (unlike `EXPECT_*`)
// expands to a bare `return` on failure -- if that fires anywhere between
// spawning `child_thread` and the driver's own explicit `child_thread.
// join()`, the `std::thread` destructor would run on a still-joinable
// thread, which calls `std::terminate()` and crashes the WHOLE test
// process (not just fails this one test), orphaning the spawned child in
// the process. This guard's destructor joins (a no-op once the explicit
// join below has already run -- `joinable()` is then false) and clears
// `kStressRootEnv` on every exit path, normal or early.
class StressChildGuard {
public:
  explicit StressChildGuard(std::thread &t) : t_(t) {}
  ~StressChildGuard() {
    if (t_.joinable()) {
      t_.join();
    }
    ::_putenv_s(kStressRootEnv, "");
  }
  StressChildGuard(const StressChildGuard &) = delete;
  StressChildGuard &operator=(const StressChildGuard &) = delete;

private:
  std::thread &t_;
};

// Child half: skipped unless the driver test below set kStressRootEnv.
TEST(CatalogWriterStressChild, InsertOneThousandRows) {
  const std::string root = stress_root_env();
  if (root.empty()) {
    GTEST_SKIP() << "child-side probe: runs only when the driver test sets " << kStressRootEnv;
  }
  auto cat = Catalog::open(root);
  ASSERT_TRUE(cat.has_value()) << (cat ? "" : cat.error().to_string());
  for (std::uint64_t i = 0; i < 1000; ++i) {
    const Status s =
        cat->register_staging(make_indexed_key(1, i), make_meta(), make_registration(i));
    ASSERT_TRUE(s.has_value()) << "child row " << i << ": " << (s ? "" : s.error().to_string());
  }
}

TEST(CatalogWriterStressDriver, TwoRealProcessesTwoThousandRowsZeroCorruption) {
  if (!stress_root_env().empty()) {
    GTEST_SKIP() << "driver test; not re-entered in the child";
  }
  const fs::path root = fresh_lake_root("stress");
  {
    // Pre-create the schema so both real writers below race genuine INSERTs
    // against an already-initialized catalog, not each other's CREATE TABLE.
    auto pre = Catalog::open(root.string());
    ASSERT_TRUE(pre.has_value()) << (pre ? "" : pre.error().to_string());
  }

  ::_putenv_s(kStressRootEnv, root.string().c_str());
  int child_rc = -1;
  std::thread child_thread(
      [&] { child_rc = run_second_process("CatalogWriterStressChild.InsertOneThousandRows"); });
  StressChildGuard guard(child_thread);

  // The PARENT's own 1000 rows, running CONCURRENTLY with the child (both
  // real OS processes/threads, both real WAL-mode SQLite connections to the
  // SAME catalog.sqlite) -- this is the actual concurrency the brief's
  // pragmas exist to make safe.
  {
    auto cat = Catalog::open(root.string());
    ASSERT_TRUE(cat.has_value());
    for (std::uint64_t i = 0; i < 1000; ++i) {
      const Status s =
          cat->register_staging(make_indexed_key(0, i), make_meta(), make_registration(i));
      ASSERT_TRUE(s.has_value())
          << "parent row " << i << ": " << (s ? "" : s.error().to_string());
    }
  }

  // Explicit join on the normal path (must happen BEFORE the verification
  // below, which needs the child's writes complete) -- `guard`'s destructor
  // then finds the thread already not-joinable and only clears the env var.
  child_thread.join();
  EXPECT_EQ(child_rc, 0) << "child process (1000 concurrent inserts) did not exit cleanly";

  const fs::path db_path = root / std::string(kCatalogDbName);
  EXPECT_EQ(raw_count(db_path, "SELECT COUNT(*) FROM tracks;"), 2000);

  auto integrity = db::Database::open(db_path.string(), db::OpenMode::ReadOnly);
  ASSERT_TRUE(integrity.has_value());
  auto stmt = integrity->prepare("PRAGMA integrity_check;");
  ASSERT_TRUE(stmt.has_value());
  auto step = stmt->step();
  ASSERT_TRUE(step.has_value());
  ASSERT_EQ(*step, db::Statement::Step::Row);
  EXPECT_EQ(stmt->column_text(0), "ok") << "concurrent writers left the catalog corrupted";
}

} // namespace
} // namespace atx::vol
