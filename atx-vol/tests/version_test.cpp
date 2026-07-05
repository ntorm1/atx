#include <gtest/gtest.h>

#include "atx/vol/version.hpp"

namespace {

TEST(Version, Query_ReturnsSemver) {
  EXPECT_EQ(atx::vol::version(), "0.1.0");
}

} // namespace
