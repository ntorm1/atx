// store_db_test.cpp — StoreDb open + schema bootstrap (persistence v2 Task 1).
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "atx/engine/store/db.hpp"
#include "atx/engine/store/schema.hpp"

namespace atxtest_store_db_test {

using atx::engine::store::StoreDb;

[[nodiscard]] std::string tmpdir() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_store_v2" /
      (std::string(info->test_suite_name()) + "_" + info->name());
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

TEST(StoreDb, MemoryOpenSetsSchemaVersion) {
  auto store = StoreDb::open_memory();
  ASSERT_TRUE(store.has_value());
  auto v = store->schema_version();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, atx::engine::store::schema::kSchemaVersion);
}

TEST(StoreDb, FileOpenIsIdempotentAcrossReopen) {
  const std::string path = tmpdir() + "/atx_dev.sqlite";
  { auto s1 = StoreDb::open(path); ASSERT_TRUE(s1.has_value()); }
  auto s2 = StoreDb::open(path); // re-open existing file: CREATE IF NOT EXISTS is a no-op
  ASSERT_TRUE(s2.has_value());
  auto v = s2->schema_version();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, atx::engine::store::schema::kSchemaVersion);
}

}  // namespace atxtest_store_db_test
