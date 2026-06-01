#include "atx/core/core.hpp"

#include <gtest/gtest.h>

TEST(CoreTest, Add) {
  EXPECT_EQ(atx::core::add(2, 3), 5);
  EXPECT_EQ(atx::core::add(-1, 1), 0);
}
