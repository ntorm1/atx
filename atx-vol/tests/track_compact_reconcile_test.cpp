// track_compact_reconcile.hpp -- track_compact's crash-recovery reconcile
// pass (Task D5 fix-round, review finding 2, backtest-production-lakehouse
// sprint).
//
// Only built when ATX_VOL_LAKEHOUSE is ON (tests/CMakeLists.txt) -- needs
// Catalog::mark_compacted/list_by_status and TrackStore::compact() actually
// compiled into atx-vol, mirroring catalog_test.cpp/track_store_test.cpp.
//
// Simulates the crash window directly (no actual process kill needed):
// register_staging + write_staging (catalog row 'staging', staging file
// present) -> compact() (deletes the staging file, folds the track into a
// real batch file) -- that IS the crash state, since it is EXACTLY what a
// process that died right after compact() but before mark_compacted leaves
// behind. reconcile_stuck_compactions must find and fix it.

#include "atx/vol/research/track_compact_reconcile.hpp"

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/backtest.hpp"             // BacktestResult
#include "atx/vol/research/track_key.hpp"   // TrackKey
#include "atx/vol/research/track_store.hpp" // TrackStore, TrackMeta, compact

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kBaseNow = 1767312000000000000LL; // 2026-01-02T00:00:00Z-ish, exact value unused
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;

[[nodiscard]] fs::path fresh_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-reconcile-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

[[nodiscard]] TrackKey make_key(std::uint8_t fill_byte) {
  TrackKey k;
  k.sha256.fill(fill_byte);
  return k;
}

// Minimal, shape-valid BacktestResult -- track_store_test.cpp's own
// make_result, trimmed to exactly what write_staging's shape validation
// needs (every mandatory series column, strictly (date, ts_ns)-ordered
// rows); the optional swap/gross-vega/step_pnl_total lanes are left empty,
// which is itself a valid contract (track_store.hpp).
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

struct StagedTrackIdentity {
  TrackKey key;
  TrackMeta meta;
};

// Stages + registers ONE track (real write_staging + real register_staging),
// leaving its catalog row 'staging' and its staging/<key>.feather present --
// the ordinary pre-compact state. Compacting it (a separate `compact()`
// call the caller makes) THEN reproduces the exact crash window this file
// tests: staging file gone, catalog row still 'staging', data durably in a
// batch file.
[[nodiscard]] StagedTrackIdentity stage_one(const fs::path &lake_root, Catalog &catalog,
                                            std::uint8_t key_fill, const std::string &underlier,
                                            const std::string &family) {
  const TrackKey key = make_key(key_fill);
  const TrackMeta meta{underlier, family};
  TrackStore store(lake_root.string());
  const BacktestResult result =
      make_result({"2026-01-02", "2026-01-03", "2026-01-04"}, 100.0 * static_cast<double>(key_fill));
  EXPECT_TRUE(store.write_staging(key, result, meta).has_value());
  EXPECT_TRUE(catalog.register_staging(key, meta, make_registration()).has_value());
  return StagedTrackIdentity{key, meta};
}

} // namespace

TEST(TrackCompactReconcileTest, StuckStagingRowIsRelocatedAndMarkedCompacted) {
  const fs::path dir = fresh_dir("basic");
  const fs::path lake_root = dir / "lake";
  auto catalog = Catalog::open(lake_root.string());
  ASSERT_TRUE(catalog.has_value());

  const StagedTrackIdentity stuck = stage_one(lake_root, *catalog, 0x11, "SPY", "strangle");

  // "The crash": compact() ran (deleted the staged input, folded the track
  // durably into a batch), but mark_compacted never did.
  auto compacted = compact(lake_root.string());
  ASSERT_TRUE(compacted.has_value()) << (compacted.has_value() ? std::string{} : compacted.error().to_string());
  ASSERT_EQ(compacted->tracks_compacted, 1u);

  auto pre_row = catalog->probe(stuck.key);
  ASSERT_TRUE(pre_row.has_value());
  ASSERT_TRUE(pre_row->has_value());
  EXPECT_EQ((*pre_row)->status, TrackStatus::Staging) << "catalog was never told compact() ran";
  EXPECT_FALSE(fs::exists(lake_root / "staging" / (stuck.key.hex() + ".feather")))
      << "compact() already deleted the staged input -- the crash-window precondition";

  auto reconciled = reconcile_stuck_compactions(*catalog, lake_root.string());
  ASSERT_TRUE(reconciled.has_value()) << (reconciled.has_value() ? std::string{} : reconciled.error().to_string());
  EXPECT_EQ(reconciled->stuck_rows_found, 1u);
  EXPECT_EQ(reconciled->rows_reconciled, 1u);

  auto post_row = catalog->probe(stuck.key);
  ASSERT_TRUE(post_row.has_value());
  ASSERT_TRUE(post_row->has_value());
  EXPECT_EQ((*post_row)->status, TrackStatus::Compacted);
  ASSERT_TRUE((*post_row)->file.has_value());
  EXPECT_EQ(*(*post_row)->file, "tracks/underlier=SPY/family=strangle/batch-000000.parquet");
  ASSERT_TRUE((*post_row)->row_group.has_value());
  EXPECT_EQ(*(*post_row)->row_group, 0);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(TrackCompactReconcileTest, GenuinelyStillStagingRowIsLeftAlone) {
  const fs::path dir = fresh_dir("not-stuck");
  const fs::path lake_root = dir / "lake";
  auto catalog = Catalog::open(lake_root.string());
  ASSERT_TRUE(catalog.has_value());

  const StagedTrackIdentity normal = stage_one(lake_root, *catalog, 0x22, "SPY", "strangle");
  // Deliberately NOT compacted -- its staging file is still present, the
  // ordinary "waiting for the next compact() run" state, not stuck.

  auto reconciled = reconcile_stuck_compactions(*catalog, lake_root.string());
  ASSERT_TRUE(reconciled.has_value()) << (reconciled.has_value() ? std::string{} : reconciled.error().to_string());
  EXPECT_EQ(reconciled->stuck_rows_found, 0u);
  EXPECT_EQ(reconciled->rows_reconciled, 0u);

  auto row = catalog->probe(normal.key);
  ASSERT_TRUE(row.has_value());
  ASSERT_TRUE(row->has_value());
  EXPECT_EQ((*row)->status, TrackStatus::Staging) << "a genuinely-still-staging row must be untouched";

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(TrackCompactReconcileTest, ConvergesAcrossTwoStuckRowsSharingOneBatch) {
  const fs::path dir = fresh_dir("two-stuck");
  const fs::path lake_root = dir / "lake";
  auto catalog = Catalog::open(lake_root.string());
  ASSERT_TRUE(catalog.has_value());

  const StagedTrackIdentity a = stage_one(lake_root, *catalog, 0x33, "QQQ", "iron_condor");
  const StagedTrackIdentity b = stage_one(lake_root, *catalog, 0x44, "QQQ", "iron_condor");

  auto compacted = compact(lake_root.string());
  ASSERT_TRUE(compacted.has_value());
  ASSERT_EQ(compacted->tracks_compacted, 2u);

  auto reconciled = reconcile_stuck_compactions(*catalog, lake_root.string());
  ASSERT_TRUE(reconciled.has_value()) << (reconciled.has_value() ? std::string{} : reconciled.error().to_string());
  EXPECT_EQ(reconciled->stuck_rows_found, 2u);
  EXPECT_EQ(reconciled->rows_reconciled, 2u);

  for (const TrackKey &key : {a.key, b.key}) {
    auto row = catalog->probe(key);
    ASSERT_TRUE(row.has_value());
    ASSERT_TRUE(row->has_value());
    EXPECT_EQ((*row)->status, TrackStatus::Compacted);
    ASSERT_TRUE((*row)->file.has_value());
    EXPECT_EQ(*(*row)->file, "tracks/underlier=QQQ/family=iron_condor/batch-000000.parquet");
    ASSERT_TRUE((*row)->row_group.has_value());
    EXPECT_EQ(*(*row)->row_group, 0);
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(TrackCompactReconcileTest, IsIdempotentOnRerun) {
  const fs::path dir = fresh_dir("idempotent");
  const fs::path lake_root = dir / "lake";
  auto catalog = Catalog::open(lake_root.string());
  ASSERT_TRUE(catalog.has_value());

  static_cast<void>(stage_one(lake_root, *catalog, 0x55, "SPY", "strangle"));
  ASSERT_TRUE(compact(lake_root.string()).has_value());

  auto first = reconcile_stuck_compactions(*catalog, lake_root.string());
  ASSERT_TRUE(first.has_value()) << (first.has_value() ? std::string{} : first.error().to_string());
  EXPECT_EQ(first->rows_reconciled, 1u);

  // A crash-recovery pass must itself be safe to just run again: the row it
  // already fixed is no longer 'staging', so a second call finds nothing
  // left to do -- not an error, not a re-attempt.
  auto second = reconcile_stuck_compactions(*catalog, lake_root.string());
  ASSERT_TRUE(second.has_value()) << (second.has_value() ? std::string{} : second.error().to_string());
  EXPECT_EQ(second->stuck_rows_found, 0u);
  EXPECT_EQ(second->rows_reconciled, 0u);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(TrackCompactReconcileTest, TrulyMissingTrackFailsClosedNotFound) {
  // A 'staging' catalog row with NO staged file and NO batch anywhere in
  // its partition -- real data loss/corruption, never silently tolerated.
  const fs::path dir = fresh_dir("missing");
  const fs::path lake_root = dir / "lake";
  auto catalog = Catalog::open(lake_root.string());
  ASSERT_TRUE(catalog.has_value());

  const TrackKey key = make_key(0x66);
  ASSERT_TRUE(
      catalog->register_staging(key, TrackMeta{"SPY", "strangle"}, make_registration()).has_value());
  // Deliberately never write_staging'd -- no staging/ file, no batch file.

  auto reconciled = reconcile_stuck_compactions(*catalog, lake_root.string());
  ASSERT_FALSE(reconciled.has_value());
  EXPECT_EQ(reconciled.error().code(), atx::core::ErrorCode::NotFound);

  std::error_code ec;
  fs::remove_all(dir, ec);
}
