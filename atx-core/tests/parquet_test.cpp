#include <atx/core/io/parquet.hpp>

#include <gtest/gtest.h>

#include <atx/core/error.hpp>

using namespace atx::core::io;

TEST(Parquet, MissingFileReturnsNotFound) {
  auto r = read_parquet("this_file_does_not_exist_12345.parquet");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::NotFound);
}
