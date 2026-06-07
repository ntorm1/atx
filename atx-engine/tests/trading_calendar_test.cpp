#include "atx/engine/quant/trading_calendar.hpp"

#include <gtest/gtest.h>

namespace {
namespace cal = atx::engine::quant;

TEST(Calendar, WeekdaysAndWeekends) {
  EXPECT_TRUE(cal::is_trading_day(2026, 6, 5));   // Fri
  EXPECT_FALSE(cal::is_trading_day(2026, 6, 6));  // Sat
  EXPECT_FALSE(cal::is_trading_day(2026, 6, 7));  // Sun
}

TEST(Calendar, FixedAndFloatingHolidays) {
  EXPECT_FALSE(cal::is_trading_day(2026, 1, 1));    // New Year (Thu)
  EXPECT_FALSE(cal::is_trading_day(2026, 1, 19));   // MLK (3rd Mon)
  EXPECT_FALSE(cal::is_trading_day(2026, 4, 3));    // Good Friday
  EXPECT_FALSE(cal::is_trading_day(2026, 5, 25));   // Memorial (last Mon)
  EXPECT_FALSE(cal::is_trading_day(2026, 11, 26));  // Thanksgiving (4th Thu)
  EXPECT_FALSE(cal::is_trading_day(2026, 12, 25));  // Christmas (Fri)
}

TEST(Calendar, WeekendObservedHolidays) {
  // Jul 4 2026 is Saturday -> observed Friday Jul 3.
  EXPECT_FALSE(cal::is_trading_day(2026, 7, 3));
  EXPECT_FALSE(cal::is_trading_day(2026, 7, 4));  // the Saturday itself: not a trading day anyway
  // New Year exception: Jan 1 2028 is Saturday -> NOT observed; Dec 31 2027 stays open.
  EXPECT_TRUE(cal::is_trading_day(2027, 12, 31)); // Fri, open
}

TEST(Calendar, TradingDaysBetween) {
  EXPECT_EQ(cal::trading_days_between(2026, 6, 5, 2026, 6, 12), 5);   // one clear week
  EXPECT_EQ(cal::trading_days_between(2026, 12, 24, 2026, 12, 28), 1); // Christmas inside
  EXPECT_EQ(cal::trading_days_between(2026, 6, 5, 2026, 6, 5), 0);     // same day
  EXPECT_EQ(cal::trading_days_between(2026, 6, 12, 2026, 6, 5), 0);    // backwards
}

TEST(Calendar, Act252Years) {
  EXPECT_DOUBLE_EQ(cal::act252_years(2026, 6, 5, 2026, 6, 5), 0.0);
  EXPECT_DOUBLE_EQ(cal::act252_years(2026, 6, 5, 2026, 6, 12), 5.0 / 252.0);
}
} // namespace
