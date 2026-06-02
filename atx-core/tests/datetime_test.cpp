// TDD test suite for atx/core/datetime.hpp (L8 `time`): Duration, Timestamp,
// civil Date, ISO-8601 format/parse, US-Eastern DST, and the NYSE trading Calendar.
//
// Known-value assertions are anchored to verifiable calendar facts (e.g. NYSE 2024
// holiday schedule, US DST transition dates, well-known Unix epoch seconds).

#include <atx/core/datetime.hpp>

#include <gtest/gtest.h>

#include <atx/core/error.hpp>

using namespace atx::core::time;
using atx::core::ErrorCode;

// =====================================================================
//  Duration
// =====================================================================
TEST(Duration, FactoriesConvertToNanos) {
  EXPECT_EQ(Duration::seconds(1).count_ns(), 1'000'000'000);
  EXPECT_EQ(Duration::milliseconds(1).count_ns(), 1'000'000);
  EXPECT_EQ(Duration::microseconds(1).count_ns(), 1'000);
  EXPECT_EQ(Duration::nanoseconds(42).count_ns(), 42);
}

TEST(Duration, CoarseAccessorsTruncate) {
  EXPECT_EQ(Duration::minutes(1).count_seconds(), 60);
  EXPECT_EQ(Duration::hours(1).count_minutes(), 60);
  EXPECT_EQ(Duration::days(1).count_hours(), 24);
  EXPECT_EQ(Duration::milliseconds(1500).count_seconds(), 1);  // truncates toward zero
}

TEST(Duration, Arithmetic) {
  EXPECT_EQ((Duration::seconds(3) + Duration::seconds(2)).count_seconds(), 5);
  EXPECT_EQ((Duration::seconds(3) - Duration::seconds(5)).count_seconds(), -2);
  EXPECT_EQ((-Duration::seconds(2)).count_seconds(), -2);
  EXPECT_EQ((Duration::seconds(2) * 3).count_seconds(), 6);
  EXPECT_EQ((3 * Duration::seconds(2)).count_seconds(), 6);
  EXPECT_EQ((Duration::seconds(6) / 2).count_seconds(), 3);
}

TEST(Duration, ZeroAndCompare) {
  EXPECT_EQ(Duration::zero().count_ns(), 0);
  EXPECT_TRUE(Duration::seconds(1) < Duration::seconds(2));
  EXPECT_TRUE(Duration::seconds(2) == Duration::milliseconds(2000));
}

TEST(Duration, AsSecondsIsFractional) {
  EXPECT_DOUBLE_EQ(Duration::milliseconds(500).as_seconds(), 0.5);
}

// =====================================================================
//  Timestamp
// =====================================================================
TEST(Timestamp, EpochIsZero) {
  EXPECT_EQ(Timestamp::epoch().unix_nanos(), 0);
  EXPECT_EQ(Timestamp::from_unix_seconds(0), Timestamp::epoch());
}

TEST(Timestamp, UnixConversions) {
  EXPECT_EQ(Timestamp::from_unix_millis(1500).unix_nanos(), 1'500'000'000);
  EXPECT_EQ(Timestamp::from_unix_seconds(10).unix_seconds(), 10);
  EXPECT_EQ(Timestamp::from_unix_micros(2'000'000).unix_seconds(), 2);
}

TEST(Timestamp, DurationArithmetic) {
  EXPECT_EQ((Timestamp::from_unix_seconds(10) + Duration::seconds(5)).unix_seconds(), 15);
  EXPECT_EQ((Timestamp::from_unix_seconds(10) - Duration::seconds(4)).unix_seconds(), 6);
  EXPECT_EQ(Timestamp::from_unix_seconds(10) - Timestamp::from_unix_seconds(3), Duration::seconds(7));
}

TEST(Timestamp, Ordering) {
  EXPECT_TRUE(Timestamp::epoch() < Timestamp::from_unix_seconds(1));
  EXPECT_TRUE(Timestamp::min() < Timestamp::max());
}

// =====================================================================
//  Civil date
// =====================================================================
TEST(Date, DaysFromCivilKnownAnchors) {
  EXPECT_EQ((Date{1970, 1, 1}).to_days(), 0);
  EXPECT_EQ((Date{1970, 1, 2}).to_days(), 1);
  EXPECT_EQ((Date{1969, 12, 31}).to_days(), -1);
  EXPECT_EQ((Date{2000, 1, 1}).to_days(), 10957);
}

TEST(Date, FromDaysRoundTrips) {
  for (std::int64_t z : {-100000, -1, 0, 1, 19000, 100000}) {
    EXPECT_EQ(Date::from_days(z).to_days(), z);
  }
}

TEST(Date, LeapYears) {
  EXPECT_TRUE(is_leap_year(2000));
  EXPECT_FALSE(is_leap_year(1900));
  EXPECT_TRUE(is_leap_year(2024));
  EXPECT_FALSE(is_leap_year(2023));
}

TEST(Date, DaysInMonth) {
  EXPECT_EQ(days_in_month(2024, 2), 29u);
  EXPECT_EQ(days_in_month(2023, 2), 28u);
  EXPECT_EQ(days_in_month(2024, 4), 30u);
  EXPECT_EQ(days_in_month(2024, 1), 31u);
}

TEST(Date, ValidityChecks) {
  EXPECT_TRUE(is_valid_date(2024, 2, 29));
  EXPECT_FALSE(is_valid_date(2023, 2, 29));
  EXPECT_FALSE(is_valid_date(2024, 13, 1));
  EXPECT_FALSE(is_valid_date(2024, 0, 1));
  EXPECT_FALSE(is_valid_date(2024, 1, 0));
}

TEST(Date, WeekdayKnownDates) {
  EXPECT_EQ(weekday(Date{1970, 1, 1}), Weekday::Thursday);
  EXPECT_EQ(weekday(Date{2024, 1, 1}), Weekday::Monday);
  EXPECT_EQ(weekday(Date{2024, 7, 4}), Weekday::Thursday);
  EXPECT_TRUE(is_weekend(Date{2024, 1, 6}));   // Saturday
  EXPECT_FALSE(is_weekend(Date{2024, 1, 8}));  // Monday
}

// =====================================================================
//  Civil time <-> Timestamp (UTC)
// =====================================================================
TEST(CivilTime, KnownUnixSecond) {
  // 2021-01-01T00:00:00Z == 1609459200 (well-known).
  EXPECT_EQ(timestamp_from_utc(2021, 1, 1, 0, 0, 0, 0).unix_seconds(), 1609459200);
}

TEST(CivilTime, DecomposeRoundTrips) {
  const Timestamp ts = timestamp_from_utc(2024, 3, 29, 13, 30, 15, 123456789);
  const CivilTime ct = to_civil_utc(ts);
  EXPECT_EQ(ct.date, (Date{2024, 3, 29}));
  EXPECT_EQ(ct.hour, 13u);
  EXPECT_EQ(ct.minute, 30u);
  EXPECT_EQ(ct.second, 15u);
  EXPECT_EQ(ct.nano, 123456789u);
}

TEST(CivilTime, EpochDecomposes) {
  const CivilTime ct = to_civil_utc(Timestamp::epoch());
  EXPECT_EQ(ct.date, (Date{1970, 1, 1}));
  EXPECT_EQ(ct.hour, 0u);
  EXPECT_EQ(ct.nano, 0u);
}

// =====================================================================
//  ISO-8601 format / parse
// =====================================================================
TEST(Iso8601, FormatFixedNanoPrecision) {
  EXPECT_EQ(to_iso8601(timestamp_from_utc(2021, 1, 1, 0, 0, 0, 0)),
            "2021-01-01T00:00:00.000000000Z");
  EXPECT_EQ(to_iso8601(timestamp_from_utc(2024, 3, 29, 13, 30, 15, 123456789)),
            "2024-03-29T13:30:15.123456789Z");
}

TEST(Iso8601, ParseRoundTrips) {
  const Timestamp ts = timestamp_from_utc(2024, 3, 29, 13, 30, 15, 123456789);
  auto parsed = from_iso8601(to_iso8601(ts));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, ts);
}

TEST(Iso8601, ParseAcceptsNoFraction) {
  auto parsed = from_iso8601("2021-01-01T00:00:00Z");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, timestamp_from_utc(2021, 1, 1, 0, 0, 0, 0));
}

TEST(Iso8601, ParseRejectsGarbage) {
  EXPECT_FALSE(from_iso8601("not-a-date").has_value());
  EXPECT_EQ(from_iso8601("not-a-date").error().code(), ErrorCode::ParseError);
  EXPECT_FALSE(from_iso8601("2021-13-01T00:00:00Z").has_value());  // bad month
  EXPECT_FALSE(from_iso8601("2021-01-01 00:00:00").has_value());   // missing T/Z
}

// =====================================================================
//  US-Eastern DST
// =====================================================================
TEST(EasternDst, WinterIsStandardTime) {
  EXPECT_FALSE(eastern_is_dst(Date{2024, 1, 15}));
  EXPECT_EQ(eastern_utc_offset(Date{2024, 1, 15}), Duration::hours(-5));
}

TEST(EasternDst, SummerIsDaylightTime) {
  EXPECT_TRUE(eastern_is_dst(Date{2024, 7, 15}));
  EXPECT_EQ(eastern_utc_offset(Date{2024, 7, 15}), Duration::hours(-4));
}

TEST(EasternDst, TransitionBoundaries2024) {
  EXPECT_FALSE(eastern_is_dst(Date{2024, 3, 9}));    // before 2nd Sunday March
  EXPECT_TRUE(eastern_is_dst(Date{2024, 3, 10}));    // DST begins
  EXPECT_TRUE(eastern_is_dst(Date{2024, 11, 2}));    // before 1st Sunday November
  EXPECT_FALSE(eastern_is_dst(Date{2024, 11, 3}));   // DST ends
}

// =====================================================================
//  Easter / Good Friday (NYSE's only moveable-feast closure)
// =====================================================================
TEST(Easter, GregorianComputus) {
  EXPECT_EQ(easter_sunday(2024), (Date{2024, 3, 31}));
  EXPECT_EQ(good_friday(2024), (Date{2024, 3, 29}));
  EXPECT_EQ(easter_sunday(2025), (Date{2025, 4, 20}));
}

// =====================================================================
//  NYSE trading Calendar — holidays
// =====================================================================
TEST(Calendar, Nyse2024FullHolidays) {
  Calendar cal;
  EXPECT_TRUE(cal.is_holiday(Date{2024, 1, 1}));    // New Year's (Mon)
  EXPECT_TRUE(cal.is_holiday(Date{2024, 1, 15}));   // MLK (3rd Mon Jan)
  EXPECT_TRUE(cal.is_holiday(Date{2024, 2, 19}));   // Washington (3rd Mon Feb)
  EXPECT_TRUE(cal.is_holiday(Date{2024, 3, 29}));   // Good Friday
  EXPECT_TRUE(cal.is_holiday(Date{2024, 5, 27}));   // Memorial (last Mon May)
  EXPECT_TRUE(cal.is_holiday(Date{2024, 6, 19}));   // Juneteenth (Wed)
  EXPECT_TRUE(cal.is_holiday(Date{2024, 7, 4}));    // Independence (Thu)
  EXPECT_TRUE(cal.is_holiday(Date{2024, 9, 2}));    // Labor (1st Mon Sep)
  EXPECT_TRUE(cal.is_holiday(Date{2024, 11, 28}));  // Thanksgiving (4th Thu Nov)
  EXPECT_TRUE(cal.is_holiday(Date{2024, 12, 25}));  // Christmas (Wed)
  EXPECT_FALSE(cal.is_holiday(Date{2024, 1, 2}));   // ordinary trading day
}

TEST(Calendar, WeekendObservanceRules) {
  Calendar cal;
  // Independence Day 2021 fell on Sunday -> observed Monday Jul 5.
  EXPECT_TRUE(cal.is_holiday(Date{2021, 7, 5}));
  // Christmas 2022 fell on Sunday -> observed Monday Dec 26.
  EXPECT_TRUE(cal.is_holiday(Date{2022, 12, 26}));
  // New Year's 2023 fell on Sunday -> observed Monday Jan 2.
  EXPECT_TRUE(cal.is_holiday(Date{2023, 1, 2}));
  // New Year's 2022 fell on Saturday -> NYSE does NOT observe the prior Friday.
  EXPECT_FALSE(cal.is_holiday(Date{2021, 12, 31}));
  EXPECT_TRUE(cal.is_trading_day(Date{2021, 12, 31}));
  // Juneteenth only an NYSE holiday from 2022 onward.
  EXPECT_FALSE(cal.is_holiday(Date{2021, 6, 18}));
}

TEST(Calendar, TradingDayPredicate) {
  Calendar cal;
  EXPECT_TRUE(cal.is_trading_day(Date{2024, 1, 2}));
  EXPECT_FALSE(cal.is_trading_day(Date{2024, 1, 1}));   // holiday
  EXPECT_FALSE(cal.is_trading_day(Date{2024, 1, 6}));   // Saturday
  EXPECT_FALSE(cal.is_trading_day(Date{2024, 3, 29}));  // Good Friday
}

TEST(Calendar, EarlyClose2024) {
  Calendar cal;
  EXPECT_TRUE(cal.is_early_close(Date{2024, 7, 3}));    // day before Independence
  EXPECT_TRUE(cal.is_early_close(Date{2024, 11, 29}));  // day after Thanksgiving
  EXPECT_TRUE(cal.is_early_close(Date{2024, 12, 24}));  // Christmas Eve
  EXPECT_FALSE(cal.is_early_close(Date{2024, 1, 2}));   // ordinary full day
  EXPECT_FALSE(cal.is_early_close(Date{2024, 12, 25}));  // holiday, not early close
}

TEST(Calendar, NextPrevTradingDay) {
  Calendar cal;
  EXPECT_EQ(cal.next_trading_day(Date{2024, 12, 24}), (Date{2024, 12, 26}));  // skip Christmas
  EXPECT_EQ(cal.next_trading_day(Date{2024, 1, 5}), (Date{2024, 1, 8}));      // skip weekend
  EXPECT_EQ(cal.prev_trading_day(Date{2024, 1, 1}), (Date{2023, 12, 29}));    // back over weekend+NY
}

// =====================================================================
//  NYSE sessions in UTC (DST-aware)
// =====================================================================
TEST(Calendar, SessionOpenCloseStandardTime) {
  Calendar cal;
  // 2024-01-02 is EST (UTC-5): 09:30 ET = 14:30Z, 16:00 ET = 21:00Z.
  EXPECT_EQ(cal.session_open(Date{2024, 1, 2}), timestamp_from_utc(2024, 1, 2, 14, 30, 0, 0));
  EXPECT_EQ(cal.session_close(Date{2024, 1, 2}), timestamp_from_utc(2024, 1, 2, 21, 0, 0, 0));
}

TEST(Calendar, SessionOpenCloseDaylightTime) {
  Calendar cal;
  // 2024-07-05 is EDT (UTC-4): 09:30 ET = 13:30Z, 16:00 ET = 20:00Z.
  EXPECT_EQ(cal.session_open(Date{2024, 7, 5}), timestamp_from_utc(2024, 7, 5, 13, 30, 0, 0));
  EXPECT_EQ(cal.session_close(Date{2024, 7, 5}), timestamp_from_utc(2024, 7, 5, 20, 0, 0, 0));
}

TEST(Calendar, EarlyCloseSessionEndsAt1pmEastern) {
  Calendar cal;
  // 2024-11-29 (day after Thanksgiving, EST): early close 13:00 ET = 18:00Z.
  EXPECT_EQ(cal.session_close(Date{2024, 11, 29}), timestamp_from_utc(2024, 11, 29, 18, 0, 0, 0));
}

// DST transitions in 2024: spring-forward is the 2nd Sunday of March (Mar 10),
// fall-back is the 1st Sunday of November (Nov 3). Both land on non-trading
// Sundays, so the offset must flip exactly on the adjacent trading days.
TEST(Calendar, SessionOffsetFlipsAcrossSpringForward) {
  Calendar cal;
  // Fri Mar 8 2024 — still EST (UTC-5): 09:30 ET = 14:30Z.
  EXPECT_EQ(cal.session_open(Date{2024, 3, 8}), timestamp_from_utc(2024, 3, 8, 14, 30, 0, 0));
  // Mon Mar 11 2024 — now EDT (UTC-4): 09:30 ET = 13:30Z.
  EXPECT_EQ(cal.session_open(Date{2024, 3, 11}), timestamp_from_utc(2024, 3, 11, 13, 30, 0, 0));
}

TEST(Calendar, SessionOffsetFlipsAcrossFallBack) {
  Calendar cal;
  // Fri Nov 1 2024 — still EDT (UTC-4): 09:30 ET = 13:30Z.
  EXPECT_EQ(cal.session_open(Date{2024, 11, 1}), timestamp_from_utc(2024, 11, 1, 13, 30, 0, 0));
  // Mon Nov 4 2024 — now EST (UTC-5): 09:30 ET = 14:30Z.
  EXPECT_EQ(cal.session_open(Date{2024, 11, 4}), timestamp_from_utc(2024, 11, 4, 14, 30, 0, 0));
}
