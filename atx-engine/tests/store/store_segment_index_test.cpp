// store_segment_index_test.cpp — segment registry + alpha->segment location (Task 7).
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/segment_index.hpp"

namespace atxtest_store_segment_index_test {
using atx::engine::store::StoreDb;
using atx::engine::store::SegmentRow;
namespace si = atx::engine::store::segment_index;

TEST(SegmentIndex, RegisterMapLocate) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  ASSERT_TRUE(si::register_segment(db, SegmentRow{"seg1", "/data/seg1.atxseg", 0xC0DEull,
                                                  /*base*/0, /*n*/3, /*crc*/0x55ull, /*fmt*/1, "run1"}).has_value());
  ASSERT_TRUE(si::map_alpha(db, 0xABCull, "seg1", /*dir_index*/2).has_value());
  auto loc = si::locate(db, 0xABCull); ASSERT_TRUE(loc.has_value());
  EXPECT_EQ(loc->segment_id, "seg1");
  EXPECT_EQ(loc->dir_index, 2);
  auto missing = si::locate(db, 0x999ull);
  EXPECT_FALSE(missing.has_value());  // unmapped -> Err(NotFound)
}

}  // namespace atxtest_store_segment_index_test
