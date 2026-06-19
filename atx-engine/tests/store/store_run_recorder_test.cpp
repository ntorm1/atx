// store_run_recorder_test.cpp — run registry write path + replay guard (Task 5).
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/run_recorder.hpp"
#include "atx/engine/store/fingerprint.hpp"

namespace atxtest_store_run_recorder_test {
using atx::engine::store::StoreDb;
using atx::engine::store::RunRow;
using atx::engine::store::RunRecorder;
using atx::engine::store::AlphaMetricsRow;
namespace fp = atx::engine::store::fingerprint;

RunRow make_run(const std::string& id, atx::u64 finger) {
  RunRow r;
  r.run_id = id; r.fingerprint = finger; r.kind = "backtest";
  r.engine_git_sha = "sha"; r.master_seed = 1; r.universe_id = "u"; r.snapshot_id = "s";
  r.fit_start = 0; r.fit_end = 100; r.bt_start = 100; r.bt_end = 200;
  r.position_mode = "book"; r.sector_neutral = true; r.rebalance_every = 5;
  r.cost_model = "flat"; r.started_at = 10;
  return r;
}

TEST(RunRecorder, FullWritePathCommits) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  auto rec = RunRecorder::begin(db, make_run("run1", 0x111ull));
  ASSERT_TRUE(rec.has_value());
  ASSERT_TRUE(rec->set_param("n_folds", "8").has_value());
  ASSERT_TRUE(rec->link_alpha(0xABCull, "admitted").has_value());
  ASSERT_TRUE(rec->record_metrics(AlphaMetricsRow{0xABCull, 1.5, 0.2, 0.1, 0.3, 0.5, 2.0}).has_value());
  ASSERT_TRUE(rec->commit(/*finished_at*/99, /*result_digest*/0xDEADull).has_value());

  auto replay = fp::is_replay(db, 0x111ull);  // now committed
  ASSERT_TRUE(replay.has_value()); EXPECT_TRUE(*replay);
}

TEST(RunRecorder, ReplayOfCommittedFingerprintRejected) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  auto r1 = RunRecorder::begin(db, make_run("run1", 0x222ull));
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1->commit(99, 0).has_value());
  // same fingerprint, different run_id -> replay -> rejected
  auto r2 = RunRecorder::begin(db, make_run("run2", 0x222ull));
  EXPECT_FALSE(r2.has_value());
}

}  // namespace atxtest_store_run_recorder_test
