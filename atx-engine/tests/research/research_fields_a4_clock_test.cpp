// The SEC / holdings clock primitives (P9 lane A4, session_clock.hpp) against the Python classes
// they port, research_fields_sec.py Calendar / Latest / Windowed: every expected value below is
// what those classes return on the same inputs (numpy 1.26.4; the Calendar with CAL_PREFIX_DAYS
// 10 and CAL_SUFFIX_DAYS 9 on a role that keeps the 2023-07-04 holiday and skips 2023-07-10).
#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/engine/research/fields/session_clock.hpp"

namespace fields = atx::engine::research::fields;
using atx::f64;
using atx::i64;
using atx::u64;
using atx::usize;

namespace {

constexpr i64 kDayNs = 86'400'000'000'000LL;
constexpr i64 kMarkNs = 22LL * 3'600'000'000'000LL;

const std::vector<i64> &role_days() {
  static const std::vector<i64> days{19541, 19542, 19543, 19544, 19545,
                                     19549, 19550, 19551, 19552};
  return days;
}

u64 bits(f64 v) { return std::bit_cast<u64>(v); }

} // namespace

TEST(ResearchFieldsA4Clock, SessionCalendarIsThePythonCalendar) {
  const auto cal = fields::SessionCalendar::create(role_days(), 10, 9);
  ASSERT_TRUE(cal.has_value()) << cal.error().message();
  const std::vector<i64> want{19531, 19534, 19535, 19536, 19537, 19538, 19541,
                              19542, 19543, 19544, 19545, 19549, 19550, 19551,
                              19552, 19555, 19556, 19557, 19558, 19559};
  EXPECT_EQ(std::vector<i64>(cal->days().begin(), cal->days().end()), want);
  EXPECT_EQ(cal->prefix(), 6U);
  EXPECT_EQ(cal->suffix(), 5U);
  for (usize u = 0; u < cal->size(); ++u) {
    EXPECT_EQ(cal->marks()[u], want[u] * kDayNs + kMarkNs);
  }
  const nlohmann::json stats{
      {"rule", "nyse-rule-v1 outside the role range, the role's sessions inside it"},
      {"prefix_days", 10},
      {"suffix_days", 9},
      {"rule_sessions_not_in_role", 1},
      {"role_sessions_not_in_rule", 1},
      {"first", "2023-06-23"},
      {"last", "2023-07-21"},
      {"prefix_sessions", 6},
      {"suffix_sessions", 5}};
  EXPECT_EQ(cal->stats(), stats);
}

TEST(ResearchFieldsA4Clock, UsableFromIsStrictlyAfterTheMark) {
  const auto cal = fields::SessionCalendar::create(role_days(), 10, 9);
  ASSERT_TRUE(cal.has_value());
  const i64 first = cal->marks().front();
  const i64 last = cal->marks().back();
  // available_at < mark(u - 1): an instant exactly at a mark is first usable one row later.
  EXPECT_EQ(cal->usable_from(first - 1), 1U);
  EXPECT_EQ(cal->usable_from(first), 2U);
  EXPECT_EQ(cal->usable_from(first + 1), 2U);
  EXPECT_EQ(cal->usable_from(1'687'989'600'000'000'000LL), 5U); // the mark of row 3
  EXPECT_EQ(cal->usable_from(last), 21U);
  EXPECT_EQ(cal->usable_from(last + 1), 21U);
  EXPECT_EQ(cal->usable_from(1'688'598'000'000'000'000LL), 10U); // 2023-07-05 23:00 UTC
  const std::vector<i64> probe{19509, 19542, 19548, 19552, 19570};
  const std::vector<usize> at_or_after{0, 7, 11, 14, 20};
  const std::vector<usize> count_through{0, 8, 11, 15, 20};
  for (usize i = 0; i < probe.size(); ++i) {
    EXPECT_EQ(cal->at_or_after(probe[i]), at_or_after[i]) << probe[i];
    EXPECT_EQ(cal->count_through(probe[i]), count_through[i]) << probe[i];
  }
}

TEST(ResearchFieldsA4Clock, SessionCalendarRefusesABadRole) {
  EXPECT_FALSE(fields::SessionCalendar::create({}, 10, 9).has_value());
  const std::vector<i64> unsorted{19542, 19541};
  EXPECT_FALSE(fields::SessionCalendar::create(unsorted, 10, 9).has_value());
  EXPECT_FALSE(fields::SessionCalendar::create(role_days(), -1, 9).has_value());
}

TEST(ResearchFieldsA4Clock, LatestRowsIsThePythonPointer) {
  auto latest = fields::LatestRows::create({0, 1, 1, 3, 3, 3, 7}, {2, 0, 2, 1, 2, 0, 1}, 3);
  ASSERT_TRUE(latest.has_value()) << latest.error().message();
  const auto at = [&latest](i64 u) {
    const auto row = latest->through(u);
    return std::vector<i64>(row.begin(), row.end());
  };
  EXPECT_EQ(at(-1), (std::vector<i64>{-1, -1, -1}));
  EXPECT_EQ(at(0), (std::vector<i64>{-1, -1, 0}));
  EXPECT_EQ(at(1), (std::vector<i64>{1, -1, 2}));
  EXPECT_EQ(at(2), (std::vector<i64>{1, -1, 2}));
  EXPECT_EQ(at(3), (std::vector<i64>{5, 3, 4}));
  EXPECT_EQ(at(2), (std::vector<i64>{5, 3, 4})); // the stream never goes back
  EXPECT_EQ(at(6), (std::vector<i64>{5, 3, 4}));
  EXPECT_EQ(at(7), (std::vector<i64>{5, 6, 4}));
  EXPECT_EQ(at(9), (std::vector<i64>{5, 6, 4}));
  EXPECT_EQ(latest->rows_passed(), 7U);
}

TEST(ResearchFieldsA4Clock, LatestRowsBeforeIsStrict) {
  auto latest = fields::LatestRows::create({10, 20, 20, 30}, {0, 1, 0, 1}, 2);
  ASSERT_TRUE(latest.has_value());
  const auto before = [&latest](i64 bound) {
    const auto row = latest->before(bound);
    return std::vector<i64>(row.begin(), row.end());
  };
  EXPECT_EQ(before(10), (std::vector<i64>{-1, -1}));
  EXPECT_EQ(before(11), (std::vector<i64>{0, -1}));
  EXPECT_EQ(before(20), (std::vector<i64>{0, -1}));
  EXPECT_EQ(before(21), (std::vector<i64>{2, 1}));
  EXPECT_EQ(before(std::numeric_limits<i64>::max()), (std::vector<i64>{2, 3}));
  EXPECT_FALSE(fields::LatestRows::create({2, 1}, {0, 0}, 1).has_value()); // not clock order
  EXPECT_FALSE(fields::LatestRows::create({1}, {3}, 2).has_value());       // column off the axis
}

TEST(ResearchFieldsA4Clock, WindowedSumsAreThePythonSumsBitForBit) {
  const std::vector<i64> enter{2, 0, 3, 5, 1, 4};
  const std::vector<i64> leave{5, 3, 3, 9, 4, 2};
  const std::vector<usize> col{0, 1, 0, 1, 0, 1};
  const std::vector<f64> val{0.1, 0.2, 0.7, 1e16, 0.3, 5.0};
  auto w = fields::WindowedSums<f64>::create(enter, leave, col, val, 2);
  ASSERT_TRUE(w.has_value()) << w.error().message();
  struct Want {
    i64 u;
    f64 a;
    f64 b;
  };
  // 0.1 + 0.3 entered then 0.3 and 0.1 left: the residue is numpy's event order's, not zero.
  const std::vector<Want> want{{0, 0.0, 0.2},
                               {1, 0.3, 0.2},
                               {2, 0.4, 0.2},
                               {3, 0.4, 0.0},
                               {4, 0.10000000000000003, 0.0},
                               {5, 2.7755575615628914e-17, 1e16},
                               {8, 2.7755575615628914e-17, 1e16},
                               {9, 2.7755575615628914e-17, 0.0},
                               {12, 2.7755575615628914e-17, 0.0}};
  for (const Want &x : want) {
    const auto s = w->at(x.u);
    EXPECT_EQ(bits(s[0]), bits(x.a)) << x.u << ": " << s[0];
    EXPECT_EQ(bits(s[1]), bits(x.b)) << x.u << ": " << s[1];
  }
  auto counts = fields::WindowedSums<i64>::create(enter, leave, col,
                                                  std::vector<i64>{1, 2, 3, 4, 5, 6}, 2);
  ASSERT_TRUE(counts.has_value());
  const auto c0 = counts->at(0);
  EXPECT_EQ(c0[0], 0);
  EXPECT_EQ(c0[1], 2);
  const auto c3 = counts->at(3);
  EXPECT_EQ(c3[0], 6);
  EXPECT_EQ(c3[1], 0);
  const auto c4 = counts->at(4);
  EXPECT_EQ(c4[0], 1);
  const auto c9 = counts->at(9);
  EXPECT_EQ(c9[0], 0);
  EXPECT_EQ(c9[1], 0);
}
