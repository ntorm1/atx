// store_universe_registry_test.cpp — universe defs + members + data snapshot (Task 3).
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/universe_registry.hpp"

namespace atxtest_store_universe_registry_test {
using atx::engine::store::StoreDb;
namespace ur = atx::engine::store::universe_registry;

TEST(UniverseRegistry, DefineAddMembersRoundTrip) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  ASSERT_TRUE(ur::define(db, "uni_sp500", "SP500", 20260101, "top500_by_cap", 0xFEEDull).has_value());
  ASSERT_TRUE(ur::add_member(db, "uni_sp500", 10).has_value());
  ASSERT_TRUE(ur::add_member(db, "uni_sp500", 20).has_value());
  auto m = ur::members(db, "uni_sp500"); ASSERT_TRUE(m.has_value());
  ASSERT_EQ(m->size(), 2u);
}

TEST(UniverseRegistry, RecordSnapshotOk) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  EXPECT_TRUE(ur::record_snapshot(s->db(), "snap_orats_0619", "ORATS", 20260619, 0x1234ull).has_value());
}

}  // namespace atxtest_store_universe_registry_test
