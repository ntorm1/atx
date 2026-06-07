#include "atx/engine/quant/osi.hpp"

#include <gtest/gtest.h>

namespace {
using atx::engine::quant::parse_osi;

TEST(Osi, ParsesStandardCall) {
  auto o = parse_osi("AAPL  260615C00322500");
  ASSERT_TRUE(o.has_value());
  EXPECT_EQ(o->root, "AAPL");
  EXPECT_EQ(o->year, 2026);
  EXPECT_EQ(o->month, 6);
  EXPECT_EQ(o->day, 15);
  EXPECT_TRUE(o->is_call);
  EXPECT_DOUBLE_EQ(o->strike, 322.5);
}

TEST(Osi, ParsesPutAndShortRoot) {
  auto o = parse_osi("XLF   261016P00047000");
  ASSERT_TRUE(o.has_value());
  EXPECT_EQ(o->root, "XLF");
  EXPECT_FALSE(o->is_call);
  EXPECT_DOUBLE_EQ(o->strike, 47.0);
  EXPECT_EQ(o->year, 2026);
  EXPECT_EQ(o->month, 10);
  EXPECT_EQ(o->day, 16);
}

TEST(Osi, ParsesDotStrippedRootAndFractionalStrike) {
  auto o = parse_osi("BRKB  271217C00058500");
  ASSERT_TRUE(o.has_value());
  EXPECT_EQ(o->root, "BRKB");
  EXPECT_DOUBLE_EQ(o->strike, 58.5);
  EXPECT_EQ(o->year, 2027);
}

TEST(Osi, RejectsWrongLength) {
  EXPECT_FALSE(parse_osi("AAPL 260615C0032250").has_value());   // 20 chars
  EXPECT_FALSE(parse_osi("").has_value());
}

TEST(Osi, RejectsBadTypeAndNonDigits) {
  EXPECT_FALSE(parse_osi("AAPL  260615X00322500").has_value());  // type X
  EXPECT_FALSE(parse_osi("AAPL  2606X5C00322500").has_value());  // non-digit date
  EXPECT_FALSE(parse_osi("AAPL  260615C003225X0").has_value());  // non-digit strike
}
} // namespace
