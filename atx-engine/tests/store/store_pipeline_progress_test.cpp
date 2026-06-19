// store_pipeline_progress_test.cpp — PipelineRecorder write path + blob helpers (Task 4).
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/pipeline_progress.hpp"

namespace atx::engine::store {

static PipelineRunRow MakeRow(atx::u64 fp) {
  PipelineRunRow r;
  r.pipeline_run_id = "run-" + std::to_string(fp);
  r.fingerprint = fp; r.stage = "discover"; r.master_seed = 7;
  r.population = 6; r.total_generations = 5; r.panel_path = "/p.bin";
  r.config_json = "{}"; r.engine_git_sha = "deadbeef"; r.created_at = 1000;
  return r;
}

TEST(PipelineRecorder, Lifecycle) {
  auto db = StoreDb::open_memory(); ASSERT_TRUE(db.has_value());
  auto rec = PipelineRecorder::begin(db->db(), MakeRow(111));
  ASSERT_TRUE(rec.has_value());
  ASSERT_TRUE(rec->save_checkpoint(0, "a\nb", 2, 1.0, 0.5, 2, 2, 100, 1001).has_value());
  ASSERT_TRUE(rec->save_checkpoint(1, "c\nd", 2, 2.0, 1.0, 4, 2, 120, 1002).has_value());
  auto blob = rec->latest_population_blob();
  ASSERT_TRUE(blob.has_value());
  EXPECT_EQ(*blob, "c\nd");
  ASSERT_TRUE(rec->complete(2000).has_value());
  // status == 'completed', last_generation == 1
  auto* s = *db->db().prepare_cached(
      "SELECT status,last_generation FROM pipeline_run WHERE fingerprint=111");
  ASSERT_EQ(*s->step(), atx::core::db::Statement::Step::Row);
  EXPECT_EQ(s->column_text(0), "completed");
  EXPECT_EQ(s->column_int(1), 1);
}

TEST(PipelineRecorder, FindResumableReturnsLatestCheckpoint) {
  auto db = StoreDb::open_memory(); ASSERT_TRUE(db.has_value());
  auto rec = PipelineRecorder::begin(db->db(), MakeRow(222));
  ASSERT_TRUE(rec.has_value());
  ASSERT_TRUE(rec->save_checkpoint(0, "x", 1, 1, 1, 1, 1, 1, 1).has_value());
  ASSERT_TRUE(rec->save_checkpoint(1, "y", 1, 1, 1, 1, 1, 1, 1).has_value());
  auto found = PipelineRecorder::find_resumable(db->db(), 222);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->last_generation, 1);
  // after complete -> not resumable
  ASSERT_TRUE(rec->complete(9).has_value());
  auto none = PipelineRecorder::find_resumable(db->db(), 222);
  EXPECT_FALSE(none.has_value());  // Err(NotFound)
}

TEST(PipelineRecorder, BlobHelpersRoundTrip) {
  std::vector<std::string> v{"rank(close)", "delta(close, 5)"};
  auto j = join_population(v);
  auto back = split_population(j);
  EXPECT_EQ(back, v);
  EXPECT_EQ(population_hash(j), population_hash(join_population(back)));
}

// Finding 1 guard: save_checkpoint on a run whose row has been deleted must
// return Err(NotFound) rather than silently writing orphaned checkpoint rows.
// (mark_failed uses the same guard; testing via delete-then-call is the
// cleanest approach since PipelineRecorder can only be obtained via begin/resume.)
TEST(PipelineRecorder, SaveCheckpointDeletedRunRejected) {
  auto db = StoreDb::open_memory(); ASSERT_TRUE(db.has_value());
  auto rec = PipelineRecorder::begin(db->db(), MakeRow(999));
  ASSERT_TRUE(rec.has_value());
  // Delete the pipeline_run row out from under the recorder
  auto* del = *db->db().prepare_cached(
      "DELETE FROM pipeline_run WHERE pipeline_run_id='run-999'");
  ASSERT_EQ(*del->step(), atx::core::db::Statement::Step::Done);
  // save_checkpoint should now return Err(NotFound)
  auto result = rec->save_checkpoint(0, "a\nb", 2, 1.0, 0.5, 2, 2, 100, 2000);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::NotFound);
}

TEST(PipelineRecorder, MarkFailedDeletedRunRejected) {
  auto db = StoreDb::open_memory(); ASSERT_TRUE(db.has_value());
  auto rec = PipelineRecorder::begin(db->db(), MakeRow(888));
  ASSERT_TRUE(rec.has_value());
  // Delete the pipeline_run row out from under the recorder
  auto* del = *db->db().prepare_cached(
      "DELETE FROM pipeline_run WHERE pipeline_run_id='run-888'");
  ASSERT_EQ(*del->step(), atx::core::db::Statement::Step::Done);
  // mark_failed should now return Err(NotFound)
  auto result = rec->mark_failed(2000, "something broke");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::NotFound);
}

}  // namespace atx::engine::store
