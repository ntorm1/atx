#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include <gtest/gtest.h>

using atx::core::Err;
using atx::core::Error;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

namespace {

Result<atx::i32> parse_positive(atx::i32 value) {
  if (value < 0) {
    return Err(ErrorCode::InvalidArgument, "negative");
  }
  return Ok(value);
}

// Exercises ATX_TRY propagation + value binding.
Result<atx::i32> doubled(atx::i32 value) {
  ATX_TRY(auto positive, parse_positive(value));
  return Ok(positive * 2);
}

} // namespace

TEST(ErrorTest, OkCarriesValue) {
  const auto r = parse_positive(5);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 5);
}

TEST(ErrorTest, ErrCarriesCodeAndMessage) {
  const auto r = parse_positive(-1);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(r.error().message(), "negative");
}

TEST(ErrorTest, ErrorToStringFormatsCodeAndMessage) {
  const Error e{ErrorCode::NotFound, "missing"};
  EXPECT_EQ(e.to_string(), "NotFound: missing");

  const Error bare{ErrorCode::Internal};
  EXPECT_EQ(bare.to_string(), "Internal");
}

TEST(ErrorTest, TryPropagatesError) {
  const auto r = doubled(-3);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(ErrorTest, TryBindsValueOnSuccess) {
  const auto r = doubled(21);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 42);
}

TEST(ErrorTest, StatusOk) {
  const Status s = Ok();
  EXPECT_TRUE(s.has_value());
}
