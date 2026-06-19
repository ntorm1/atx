// store_alpha_catalog_test.cpp — identity + lineage + dedup (Task 2).
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/alpha_catalog.hpp"

namespace atxtest_store_alpha_catalog_test {
using atx::engine::store::StoreDb;
namespace cat = atx::engine::store::alpha_catalog;

TEST(AlphaCatalog, UpsertThenExistsAndDedup) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  ASSERT_TRUE(cat::upsert(db, 0xABCDull, 1, "rank(close)", 100, "run1").has_value());
  auto ex = cat::exists(db, 0xABCDull); ASSERT_TRUE(ex.has_value()); EXPECT_TRUE(*ex);
  // re-upsert same canon_hash is a no-op (dedup), not an error
  EXPECT_TRUE(cat::upsert(db, 0xABCDull, 1, "rank(close)", 100, "run1").has_value());
  auto missing = cat::exists(db, 0x9999ull); ASSERT_TRUE(missing.has_value()); EXPECT_FALSE(*missing);
}

TEST(AlphaCatalog, LineageParentsRoundTrip) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  ASSERT_TRUE(cat::add_lineage(db, /*child*/5, /*parent*/2, /*op*/7, /*seed*/42).has_value());
  ASSERT_TRUE(cat::add_lineage(db, 5, 3, 7, 42).has_value());
  auto ps = cat::parents(db, 5); ASSERT_TRUE(ps.has_value());
  ASSERT_EQ(ps->size(), 2u);
}

}  // namespace atxtest_store_alpha_catalog_test
