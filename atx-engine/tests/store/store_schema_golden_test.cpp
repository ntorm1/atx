// store_schema_golden_test.cpp — guard against accidental schema drift (Task 9).
// Asserts the exact set of table names created by schema::create_all. Adding/removing
// a table is a deliberate act that must update this golden set.
#include <map>
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
  // Merge union: main's clustering table (cluster_panel) AND the resumable-discover
  // pipeline tables (pipeline_*). The merged schema::create_all builds all of them.
  const std::set<std::string> golden = {
    "alpha", "alpha_event", "alpha_lineage", "alpha_metrics", "cluster_panel", "conviction",
    "data_snapshot", "env_config", "eval_fold", "lifecycle_journal", "pipeline_checkpoint",
    "pipeline_event", "pipeline_iteration", "pipeline_log", "pipeline_run", "promotion_ledger",
    "run", "run_alpha", "run_param", "schema_meta", "segment", "segment_alpha", "universe",
    "universe_member",
  };
  EXPECT_EQ(got, golden);
}

// cluster_panel.asof_date and created_at carry ISO strings ("2026-06-19",
// "2026-06-19T00:00:00Z") in ClusterPanelRecord/ParamsKey and are bound/read as
// TEXT. The declared column types must therefore be TEXT, not INTEGER. This guards
// the corrected schema against a regression back to the type mismatch.
TEST(StoreSchema, ClusterPanelDateColumnsAreText) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  auto stmt = db.prepare("SELECT name, type FROM pragma_table_info('cluster_panel')");
  ASSERT_TRUE(stmt.has_value());
  std::map<std::string, std::string> col_type;
  for (;;) {
    auto step = stmt->step(); ASSERT_TRUE(step.has_value());
    if (*step == atx::core::db::Statement::Step::Done) break;
    col_type.emplace(std::string(stmt->column_text(0)), std::string(stmt->column_text(1)));
  }
  EXPECT_EQ(col_type["asof_date"], "TEXT") << "asof_date stores an ISO date string";
  EXPECT_EQ(col_type["created_at"], "TEXT") << "created_at stores an ISO timestamp string";
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
