#include "atx/engine/engine.hpp"

#include <gtest/gtest.h>

TEST(EngineTest, Step) {
  EXPECT_EQ(atx::engine::step(0), 1);
  EXPECT_EQ(atx::engine::step(41), 42);
}
