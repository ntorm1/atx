#include <gtest/gtest.h>

#include "atx/tsdb/version.hpp"

TEST(Scaffold_VersionString_NonEmpty, ReturnsName) {
  EXPECT_EQ(atx::tsdb::version(), "atx-tsdb 0.1.0");
}
