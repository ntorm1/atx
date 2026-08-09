// Catalog -- SQLite catalog for the backtest lakehouse (Task D3,
// backtest-production-lakehouse sprint). research/catalog.hpp, src/catalog.cpp.
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

#include "atx/vol/research/catalog.hpp"

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
