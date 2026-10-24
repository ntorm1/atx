// Allocation budget of the dividend-schedule inference: ZERO, on every path.
//
// Counted with tests/support/alloc_counter.* (thread-local armed tally over a
// replaced global operator new), linked ONLY into atx-vol-dividend-schedule-tests,
// never into the monolithic atx-vol-tests (which has its own replacement).
#include "atx/vol/api/marketdata/dividend_schedule.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "support/alloc_counter.hpp"

namespace atx::vol {
namespace {

constexpr std::int32_t kValuationDay = 20740;

// Two bracketed dividends set a 91-day cadence; four more sit 60 days apart in
// one sparse window too short for that cadence, so the peel, count, split AND
// its spacing bisection all run, then verify.
std::vector<DividendLadderExpiry> split_ladder() {
  const int event_days[] = {20, 111, 150, 210, 270, 330};
  const int expiry_days[] = {7, 30, 60, 120, 340};
  std::vector<DividendLadderExpiry> out;
  for (const int d : expiry_days) {
    double sum = 0.0;
    double pv = 0.0;
    for (const int e : event_days) {
      if (e <= d) {
        sum += 0.5;
        pv += 0.5 * std::exp(-0.045 * e / 365.0);
      }
    }
    out.push_back({kValuationDay + d, d / 365.0, 0.045, sum, pv});
  }
  return out;
}

TEST(DividendScheduleAllocation, InferenceAndBothConversionsAllocateNothing) {
  const auto input = split_ladder();
  DividendScheduleResult schedule;
  std::array<CashDividend, kDividendScheduleMaxEvents> events{};
  std::array<DividendEvent, kDividendScheduleMaxEvents> instants{};
  DividendExpiryEvents life;
  std::size_t n_instants = 0;
  const std::uint64_t allocations = testkit::count_allocations([&] {
    schedule = infer_dividend_schedule(kValuationDay, input);
    life = dividend_schedule_expiry_events(schedule, kValuationDay + 340, 340 / 365.0, events);
    n_instants = dividend_schedule_instants(schedule, instants);
  });
  EXPECT_EQ(allocations, 0u);
  // Guard against measuring a path that did not run.
  EXPECT_EQ(schedule.status, DividendScheduleStatus::Ambiguous);
  EXPECT_EQ(schedule.split_increments, 1u);
  EXPECT_EQ(life.count, 6u);
  EXPECT_EQ(n_instants, 6u);
}

TEST(DividendScheduleAllocation, RefusalsAllocateNothing) {
  auto input = split_ladder();
  input[3].dividend_sum = 0.1; // decreasing: Inconsistent
  DividendScheduleResult inconsistent;
  DividendScheduleResult invalid;
  const std::uint64_t allocations = testkit::count_allocations([&] {
    inconsistent = infer_dividend_schedule(kValuationDay, input);
    invalid = infer_dividend_schedule(kValuationDay + 1000, input);
  });
  EXPECT_EQ(allocations, 0u);
  EXPECT_EQ(inconsistent.status, DividendScheduleStatus::Inconsistent);
  EXPECT_EQ(invalid.status, DividendScheduleStatus::Invalid);
}

} // namespace
} // namespace atx::vol
