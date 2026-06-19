// store_schema_golden_test.cpp — guard against accidental schema drift (Task 9).
// Asserts the exact set of table names created by schema::create_all. Adding/removing
// a table is a deliberate act that must update this golden set.
#include <set>
#include <string>
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"

namespace atxtest_store_schema_golden_test {
using atx::engine::store::StoreDb;

TEST(StoreSchema, GoldenTableSet) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  auto stmt = db.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
  ASSERT_TRUE(stmt.has_value());
  std::set<std::string> got;
  for (;;) {
    auto step = stmt->step(); ASSERT_TRUE(step.has_value());
    if (*step == atx::core::db::Statement::Step::Done) break;
    got.insert(std::string(stmt->column_text(0)));
  }
  const std::set<std::string> golden = {
    "alpha", "alpha_event", "alpha_lineage", "alpha_metrics", "conviction", "data_snapshot",
    "env_config", "eval_fold", "lifecycle_journal", "pipeline_checkpoint", "pipeline_event",
    "pipeline_iteration", "pipeline_log", "pipeline_run", "promotion_ledger", "run", "run_alpha",
    "run_param", "schema_meta", "segment", "segment_alpha", "universe", "universe_member",
  };
  EXPECT_EQ(got, golden);
}

TEST(StoreSchemaGolden, PipelineTablesPresentAtV2) {
  auto db = atx::engine::store::StoreDb::open_memory();
  ASSERT_TRUE(db.has_value());
  auto v = db->schema_version(); ASSERT_TRUE(v.has_value()); EXPECT_EQ(*v, 2);
  for (const char* t : {"pipeline_run", "pipeline_checkpoint", "pipeline_iteration",
                        "pipeline_event", "pipeline_log"}) {
    auto stmt = db->db().prepare(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1");
    ASSERT_TRUE(stmt.has_value());
    ASSERT_TRUE(stmt->bind(1, t).has_value());
    auto step = stmt->step(); ASSERT_TRUE(step.has_value());
    EXPECT_EQ(*step, atx::core::db::Statement::Step::Row) << "missing " << t;
  }
}

TEST(StoreSchemaGolden, CreateAllIdempotent) {
  auto db = atx::engine::store::StoreDb::open_memory(); ASSERT_TRUE(db.has_value());
  ASSERT_TRUE(atx::engine::store::schema::create_all(db->db()).has_value());  // 2nd call no-op
}

}  // namespace atxtest_store_schema_golden_test
