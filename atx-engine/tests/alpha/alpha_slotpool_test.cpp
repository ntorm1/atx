#include "atx/engine/alpha/bytecode.hpp"
#include <gtest/gtest.h>

namespace atxtest_alpha_slotpool_test {

using atx::engine::alpha::SlotId;
using atx::engine::alpha::detail::SlotPool;

TEST(SlotPoolBlock, ContiguousBlockThenRelease) {
  SlotPool pool;
  const SlotId base = pool.acquire_block(3);
  EXPECT_EQ(pool.peak(), 3u);
  pool.release_block(base, 3);
  const SlotId reused = pool.acquire_block(3); // same-size block reuses freed slots
  EXPECT_EQ(reused, base);
  EXPECT_EQ(pool.peak(), 3u);
}
TEST(SlotPoolBlock, SingleAcquireUnaffected) {
  SlotPool pool;
  const SlotId a = pool.acquire();
  const SlotId b = pool.acquire();
  EXPECT_NE(a, b);
}


}  // namespace atxtest_alpha_slotpool_test
