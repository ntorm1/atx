// Discrete dividend schedule inference (api/marketdata/dividend_schedule.hpp):
// synthetic ladders built from the PV identity the module inverts, refusals,
// clock mapping, the shared route, and a priced American example.
#include "atx/vol/api/marketdata/dividend_schedule.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/dividend.hpp"

namespace atx::vol {
namespace {

constexpr std::int32_t kValuationDay = 20740; // 2026-10-14

struct TrueEvent {
  double amount;
  int day; // calendar days after the valuation date
};

// The vendor identity: sum and PV per expiry, PV at that expiry's own rate.
// Distinct per-expiry rates so a peel that reused one rate would be caught.
std::vector<DividendLadderExpiry> ladder(const std::vector<TrueEvent> &events,
                                         const std::vector<int> &expiry_days,
                                         double base_rate = 0.045, double rate_slope = 2.0e-4) {
  std::vector<DividendLadderExpiry> out;
  for (std::size_t k = 0; k < expiry_days.size(); ++k) {
    const int d = expiry_days[k];
    const double rate = base_rate + rate_slope * static_cast<double>(k);
    double sum = 0.0;
    double pv = 0.0;
    for (const TrueEvent &e : events) {
      if (e.day <= d) {
        sum += e.amount;
        pv += e.amount * std::exp(-rate * e.day / 365.0);
      }
    }
    // A business-time clock a little faster than ACT/365, as the export's is.
    out.push_back({kValuationDay + d, 1.02 * d / 365.0, rate, sum, pv});
  }
  return out;
}

void expect_reprices(const DividendScheduleResult &s,
                     const std::vector<DividendLadderExpiry> &input, double tol) {
  for (const auto &e : input) {
    if (e.expiry_day > s.resolved_through_day) continue;
    const double horizon = e.expiry_day - s.valuation_day;
    double sum = 0.0;
    double pv = 0.0;
    for (const auto &ev : s.view()) {
      if (ev.days <= horizon) {
        sum += ev.amount;
        pv += ev.amount * std::exp(-e.rate * ev.days / 365.0);
      }
    }
    EXPECT_NEAR(sum, e.dividend_sum, tol) << "expiry " << e.expiry_day;
    EXPECT_NEAR(pv, e.dividend_pv, tol) << "expiry " << e.expiry_day;
  }
}

TEST(DividendSchedule, OneEventRecoversItsIntegerDayAndAmount) {
  const auto input = ladder({{0.82, 12}}, {7, 14, 21, 35});
  const auto s = infer_dividend_schedule(kValuationDay, input);
  ASSERT_EQ(s.status, DividendScheduleStatus::Ok);
  EXPECT_EQ(s.reason, DividendScheduleReason::None);
  ASSERT_EQ(s.n_events, 1u);
  EXPECT_EQ(s.events[0].days, 12.0);
  EXPECT_EQ(s.events[0].ex_day, kValuationDay + 12);
  EXPECT_NEAR(s.events[0].amount, 0.82, 1e-14);
  EXPECT_EQ(s.events[0].kind, DividendEventKind::Exact);
  EXPECT_EQ(s.events[0].increment_expiry_day, kValuationDay + 14);
  EXPECT_EQ(s.increments, 1u);
  EXPECT_EQ(s.exact_increments, 1u);
  EXPECT_EQ(s.resolved_through_day, kValuationDay + 35);
  expect_reprices(s, input, 1e-12);
}

TEST(DividendSchedule, TwoBracketedEventsAreBothExactAndSetTheCadence) {
  const auto input = ladder({{0.5, 12}, {0.52, 103}}, {7, 30, 60, 120, 200});
  const auto s = infer_dividend_schedule(kValuationDay, input);
  ASSERT_EQ(s.status, DividendScheduleStatus::Ok);
  ASSERT_EQ(s.n_events, 2u);
  EXPECT_EQ(s.events[0].days, 12.0);
  EXPECT_EQ(s.events[1].days, 103.0);
  EXPECT_NEAR(s.events[1].amount, 0.52, 1e-14);
  EXPECT_EQ(s.cadence_days, 91.0);
  EXPECT_DOUBLE_EQ(s.unit_amount, 0.51);
  expect_reprices(s, input, 1e-12);
}

TEST(DividendSchedule, AllZeroSumsAreNoDividendsAndStillCovered) {
  const auto input = ladder({}, {7, 30, 90});
  const auto s = infer_dividend_schedule(kValuationDay, input);
  EXPECT_EQ(s.status, DividendScheduleStatus::NoDividends);
  EXPECT_EQ(s.n_events, 0u);
  EXPECT_TRUE(s.covers(kValuationDay + 90));
  EXPECT_FALSE(s.covers(kValuationDay + 91));
  EXPECT_FALSE(s.covers(kValuationDay));
}

// Four quarterly dividends between two sparse expiries leave only their sum
// and PV. With the cadence known from the bracketed events, the declared rule
// reproduces the true equally spaced dates; the result is flagged, not Ok.
TEST(DividendSchedule, EventsBetweenSparseExpiriesAreSplitAndFlaggedAmbiguous) {
  const std::vector<TrueEvent> truth{{0.5, 20}, {0.5, 111}, {0.5, 202},
                                     {0.5, 293}, {0.5, 384}, {0.5, 475}};
  const auto input = ladder(truth, {30, 120, 500});
  const auto s = infer_dividend_schedule(kValuationDay, input);
  ASSERT_EQ(s.status, DividendScheduleStatus::Ambiguous);
  EXPECT_EQ(s.reason, DividendScheduleReason::None);
  EXPECT_EQ(s.increments, 3u);
  EXPECT_EQ(s.exact_increments, 2u);
  EXPECT_EQ(s.split_increments, 1u);
  EXPECT_EQ(s.cadence_days, 91.0);
  ASSERT_EQ(s.n_events, 6u);
  for (std::size_t i = 2; i < 6; ++i) {
    EXPECT_EQ(s.events[i].kind, DividendEventKind::SumSplit);
    EXPECT_EQ(s.events[i].increment_events, 4u);
    EXPECT_NEAR(s.events[i].amount, 0.5, 1e-14);
    EXPECT_NEAR(s.events[i].days, truth[i].day, 1e-6);
  }
  expect_reprices(s, input, 1e-12);
}

TEST(DividendSchedule, SplitWithoutCadenceSpacesEventsInsideTheWindowAndKeepsThePv) {
  // One bracketed dividend (unit 0.3), then two units in a later window: no two
  // adjacent exactly dated increments, so no cadence; spacing = window / n.
  const auto input = ladder({{0.3, 10}, {0.3, 70}, {0.3, 160}}, {15, 40, 200});
  const auto s = infer_dividend_schedule(kValuationDay, input);
  ASSERT_EQ(s.status, DividendScheduleStatus::Ambiguous);
  EXPECT_EQ(s.cadence_days, 0.0);
  ASSERT_EQ(s.n_events, 3u);
  EXPECT_EQ(s.events[0].kind, DividendEventKind::Exact);
  for (std::size_t i = 1; i < 3; ++i) {
    EXPECT_EQ(s.events[i].kind, DividendEventKind::SumSplit);
    EXPECT_GE(s.events[i].days, 41.0);
    EXPECT_LE(s.events[i].days, 200.0);
  }
  EXPECT_LT(s.events[1].days, s.events[2].days);
  expect_reprices(s, input, 1e-12);
}

TEST(DividendSchedule, SpecialDividendInANarrowWindowIsNotReadAsARunOfUnits) {
  // Quarterly 0.25, then a 5.00 special inside a 14-day window: twenty units by
  // amount, but the window holds one event at half the cadence.
  const auto input =
      ladder({{0.25, 10}, {0.25, 101}, {5.0, 150}, {0.25, 192}}, {20, 110, 140, 154, 200});
  const auto s = infer_dividend_schedule(kValuationDay, input);
  ASSERT_EQ(s.status, DividendScheduleStatus::Ok);
  ASSERT_EQ(s.n_events, 4u);
  EXPECT_EQ(s.events[2].days, 150.0);
  EXPECT_NEAR(s.events[2].amount, 5.0, 1e-13);
  EXPECT_EQ(s.events[2].kind, DividendEventKind::Exact);
}

// The review probe (dividend-a402d615-review-v1, M1): quarterly 0.25 at days 10
// and 101, then a special alone in the 89-day window (110, 200], which holds two
// events at half the 91-day cadence. Four (or eight) units cannot be a run of
// two, so the special stays ONE event at its true date, flagged; before this
// rule it became two 0.50 SumSplit events at days 111.0 and 189.19.
TEST(DividendSchedule, AnIncrementWithMoreUnitsThanItsWindowHoldsIsKeptWholeAndFlagged) {
  for (const double special : {1.0, 2.0}) {
    const auto input = ladder({{0.25, 10}, {0.25, 101}, {special, 150}}, {20, 110, 200});
    const auto s = infer_dividend_schedule(kValuationDay, input);
    ASSERT_EQ(s.status, DividendScheduleStatus::Ambiguous) << special;
    EXPECT_EQ(s.cadence_days, 91.0);
    EXPECT_DOUBLE_EQ(s.unit_amount, 0.25);
    ASSERT_EQ(s.n_events, 3u) << special;
    EXPECT_EQ(s.events[2].kind, DividendEventKind::NonIntegerDay);
    EXPECT_EQ(s.events[2].increment_events, 1u);
    EXPECT_NEAR(s.events[2].amount, special, 1e-13);
    EXPECT_NEAR(s.events[2].days, 150.0, 1e-6);
    EXPECT_EQ(s.split_increments, 0u);
    EXPECT_EQ(s.non_integer_increments, 1u);
    expect_reprices(s, input, 1e-12);
  }
}

// The limit of that rule, pinned: a special worth no more units than its window
// holds apart (0.50 = two units in the same window) is indistinguishable by
// amount from two regular dividends, and is split as a run, flagged.
TEST(DividendSchedule, ASpecialWorthNoMoreUnitsThanItsWindowHoldsIsSplitAsARun) {
  const auto input = ladder({{0.25, 10}, {0.25, 101}, {0.5, 150}}, {20, 110, 200});
  const auto s = infer_dividend_schedule(kValuationDay, input);
  ASSERT_EQ(s.status, DividendScheduleStatus::Ambiguous);
  ASSERT_EQ(s.n_events, 4u);
  for (std::size_t i = 2; i < 4; ++i) {
    EXPECT_EQ(s.events[i].kind, DividendEventKind::SumSplit);
    EXPECT_EQ(s.events[i].increment_events, 2u);
    EXPECT_NEAR(s.events[i].amount, 0.25, 1e-14);
  }
  EXPECT_EQ(s.split_increments, 1u);
  expect_reprices(s, input, 1e-12);
}

// The review probe (M2): a four-dividend run split in (120, 480], then a lone
// 0.5 dividend at day 566 in (480, 600]. Pass 1 dates day 566 against a single
// provisional stand-in for the run, so with a steep per-expiry rate slope it
// read as a non-integer day (30 and 80 bp per expiry) although the final events
// place it at 566.000000. Its kind is now decided from the final events, so it
// is Exact at every slope.
TEST(DividendSchedule, ASingleEventAfterASplitIsClassifiedAgainstTheFinalEvents) {
  const std::vector<TrueEvent> truth{{0.5, 20},  {0.5, 111}, {0.5, 202}, {0.5, 293},
                                     {0.5, 384}, {0.5, 475}, {0.5, 566}};
  for (const double slope : {2.0e-4, 30.0e-4, 80.0e-4}) {
    const auto input = ladder(truth, {30, 120, 480, 600}, 0.045, slope);
    const auto s = infer_dividend_schedule(kValuationDay, input);
    ASSERT_EQ(s.status, DividendScheduleStatus::Ambiguous) << slope;
    ASSERT_EQ(s.n_events, 7u) << slope;
    for (std::size_t i = 2; i < 6; ++i) EXPECT_EQ(s.events[i].kind, DividendEventKind::SumSplit);
    EXPECT_EQ(s.events[6].kind, DividendEventKind::Exact) << slope;
    EXPECT_EQ(s.events[6].days, 566.0) << slope;
    EXPECT_EQ(s.exact_increments, 3u) << slope;
    EXPECT_EQ(s.non_integer_increments, 0u) << slope;
    expect_reprices(s, input, 1e-9);
  }
}

TEST(DividendSchedule, RoundTripOfAConstructedTableReproducesEverySumAndPv) {
  const std::vector<TrueEvent> truth{{0.24, 3},  {0.24, 33},  {0.24, 64},  {0.26, 94},
                                     {0.26, 125}, {0.26, 155}, {0.26, 186}, {0.26, 216},
                                     {0.27, 247}, {0.27, 277}, {0.27, 308}};
  const auto input = ladder(truth, {2, 9, 16, 37, 65, 93, 128, 156, 219, 338});
  const auto s = infer_dividend_schedule(kValuationDay, input);
  ASSERT_NE(s.status, DividendScheduleStatus::Invalid);
  ASSERT_NE(s.status, DividendScheduleStatus::Inconsistent);
  EXPECT_EQ(s.resolved_through_day, kValuationDay + 338);
  // A split reproduces its PV exactly at ITS expiry's rate; at the next
  // expiry's (here 2 bp higher) rate its spacing differs from the truth only to
  // second order, and an exactly dated increment there cannot absorb it: the
  // measured residual is ~1e-10, far inside the 1e-5 fine tolerance.
  expect_reprices(s, input, 1e-9);
  EXPECT_LT(s.max_pv_relative_residual, 1e-9);
  EXPECT_EQ(s.pv_residual_exceedances, 0u);
}

// The SpiderRock 2026-10-14 15:00 ASML ladder exactly as quotes.csv carries it
// (float32 values widened to double). Five quarterly ex-dates are recovered to
// integer days; the two two-unit gaps are split and flagged.
TEST(DividendSchedule, RealFloat32LadderRecoversQuarterlyExDates) {
  const std::vector<DividendLadderExpiry> input{
      {20742, 0.00939783826470375, 0.044896867126226425, 0, 0},
      {20749, 0.028985416516661644, 0.0448814332485199, 0, 0},
      {20756, 0.04857299476861954, 0.04490809515118599, 3.1770999431610107, 3.172412633895874},
      {20763, 0.06820424646139145, 0.04496052861213684, 3.1770999431610107, 3.1724071502685547},
      {20770, 0.0877918228507042, 0.045037999749183655, 3.1770999431610107, 3.1723990440368652},
      {20777, 0.10737939924001694, 0.04513823240995407, 3.1770999431610107, 3.1723885536193848},
      {20784, 0.12320990115404129, 0.045253366231918335, 3.1770999431610107, 3.1723766326904297},
      {20805, 0.18197263777256012, 0.04564831405878067, 3.1770999431610107, 3.172335386276245},
      {20833, 0.25411561131477356, 0.04619360715150833, 3.1770999431610107, 3.172278642654419},
      {20868, 0.347152978181839, 0.046780478209257126, 6.3541998863220215, 6.307651996612549},
      {20896, 0.425459623336792, 0.04721422493457794, 6.3541998863220215, 6.307222843170166},
      {20924, 0.5013596415519714, 0.047639306634664536, 6.3541998863220215, 6.306802272796631},
      {20986, 0.6716992855072021, 0.0485491007566452, 9.531299591064453, 9.401657104492188},
      {21078, 0.9224853515625, 0.04965313524007797, 12.708399772644043, 12.454605102539062},
      {21169, 1.173410415649414, 0.050364717841148376, 15.885499954223633, 15.467073440551758},
      {21204, 1.2664477825164795, 0.05055979639291763, 15.885499954223633, 15.465483665466309},
      {21351, 1.6703925132751465, 0.05112478509545326, 22.239700317382812, 21.37381362915039},
      {21533, 2.1672985553741455, 0.05153101310133934, 28.59389877319336, 27.126358032226562},
      {21568, 2.257885694503784, 0.0515890046954155, 28.59389877319336, 27.124767303466797}};
  const auto s = infer_dividend_schedule(kValuationDay, input);
  ASSERT_EQ(s.status, DividendScheduleStatus::Ambiguous);
  ASSERT_EQ(s.n_events, 9u);
  const double exact_days[] = {12, 103, 195, 286, 377};
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(s.events[i].kind, DividendEventKind::Exact) << i;
    EXPECT_EQ(s.events[i].days, exact_days[i]) << i;
  }
  for (std::size_t i = 5; i < 9; ++i) EXPECT_EQ(s.events[i].kind, DividendEventKind::SumSplit);
  EXPECT_EQ(s.split_increments, 2u);
  EXPECT_LT(s.max_pv_relative_residual, 1e-5);
  EXPECT_EQ(s.resolved_through_day, 21568);
}

TEST(DividendSchedule, DayRoundingToleranceSeparatesExactFromNonIntegerDays) {
  // Shift the PV so the implied single-event day is 12 + offset.
  const auto shifted = [](double offset) {
    auto input = ladder({{1.0, 12}}, {7, 30});
    const double rate = input[1].rate;
    input[1].dividend_pv = std::exp(-rate * (12.0 + offset) / 365.0);
    return input;
  };
  const auto near = infer_dividend_schedule(kValuationDay, shifted(0.03));
  ASSERT_EQ(near.status, DividendScheduleStatus::Ok);
  EXPECT_EQ(near.events[0].days, 12.0);
  const auto far = infer_dividend_schedule(kValuationDay, shifted(0.2));
  ASSERT_EQ(far.status, DividendScheduleStatus::Ambiguous);
  EXPECT_EQ(far.events[0].kind, DividendEventKind::NonIntegerDay);
  EXPECT_NEAR(far.events[0].days, 12.2, 1e-9);
  EXPECT_EQ(far.non_integer_increments, 1u);
  DividendScheduleConfig loose;
  loose.day_tolerance = 0.25;
  EXPECT_EQ(infer_dividend_schedule(kValuationDay, shifted(0.2), loose).status,
            DividendScheduleStatus::Ok);
}

TEST(DividendSchedule, IdenticalDuplicatesMergeAndInputOrderDoesNotMatter) {
  auto input = ladder({{0.82, 12}}, {7, 14, 21});
  std::vector<DividendLadderExpiry> shuffled{input[2], input[0], input[1], input[0]};
  const auto s = infer_dividend_schedule(kValuationDay, shuffled);
  ASSERT_EQ(s.status, DividendScheduleStatus::Ok);
  EXPECT_EQ(s.expiries, 3u);
  EXPECT_EQ(s.events[0].days, 12.0);
}

TEST(DividendSchedule, MalformedInputIsInvalidWithNoEvents) {
  const auto base = ladder({{0.82, 12}}, {7, 14});
  const auto refused = [&](auto mutate, DividendScheduleReason reason) {
    auto input = base;
    mutate(input);
    const auto s = infer_dividend_schedule(kValuationDay, input);
    EXPECT_EQ(s.status, DividendScheduleStatus::Invalid) << to_string(reason);
    EXPECT_EQ(s.reason, reason) << to_string(s.reason);
    EXPECT_EQ(s.n_events, 0u);
    EXPECT_FALSE(s.covers(kValuationDay + 7));
  };
  refused([](auto &v) { v[1].rate = std::nan(""); }, DividendScheduleReason::NonFiniteInput);
  refused([](auto &v) { v[1].dividend_pv = -1.0; }, DividendScheduleReason::NegativeInput);
  refused([](auto &v) { v[0].years = 0.0; }, DividendScheduleReason::NegativeInput);
  refused([](auto &v) { v[0].expiry_day = kValuationDay; },
          DividendScheduleReason::ExpiryNotAfterValuation);
  refused([](auto &v) { v[0].expiry_day = kValuationDay + 40000; },
          DividendScheduleReason::ExpiryBeyondHorizon);
  refused([](auto &v) { v.push_back(v[1]); v.back().dividend_pv *= 1.01; },
          DividendScheduleReason::DuplicateExpiryConflict);
  std::vector<DividendLadderExpiry> many;
  for (int d = 1; d <= static_cast<int>(kDividendScheduleMaxExpiries) + 1; ++d) {
    many.push_back({kValuationDay + d, d / 365.0, 0.045, 0.0, 0.0});
  }
  EXPECT_EQ(infer_dividend_schedule(kValuationDay, many).reason,
            DividendScheduleReason::TooManyExpiries);
  DividendScheduleConfig bad;
  bad.day_basis = 0.0;
  EXPECT_EQ(infer_dividend_schedule(kValuationDay, base, bad).reason,
            DividendScheduleReason::InvalidConfig);
}

TEST(DividendSchedule, ContradictionsAreInconsistentAndKeepTheConsistentPrefix) {
  const auto check = [](std::vector<DividendLadderExpiry> input, DividendScheduleReason reason,
                        std::int32_t failed, std::size_t prefix_events) {
    const auto s = infer_dividend_schedule(kValuationDay, input);
    EXPECT_EQ(s.status, DividendScheduleStatus::Inconsistent) << to_string(reason);
    EXPECT_EQ(s.reason, reason) << to_string(s.reason);
    EXPECT_EQ(s.failed_expiry_day, failed);
    EXPECT_EQ(s.n_events, prefix_events);
    EXPECT_FALSE(s.covers(failed));
    return s;
  };
  auto input = ladder({{0.5, 12}, {0.5, 103}}, {30, 60, 120});
  // A decreasing cumulative sum.
  auto decreasing = input;
  decreasing[2].dividend_sum = 0.25;
  const auto s = check(decreasing, DividendScheduleReason::DecreasingSum, kValuationDay + 120, 1);
  EXPECT_EQ(s.resolved_through_day, kValuationDay + 60);
  EXPECT_TRUE(s.covers(kValuationDay + 60));
  // PV above the sum at a positive rate: the cash would be paid before today.
  auto early = input;
  early[2].dividend_pv = early[1].dividend_pv + 0.5 * 1.001;
  check(early, DividendScheduleReason::ExDateOutsideBracket, kValuationDay + 120, 1);
  // PV so low the increment would be paid after its own expiry.
  auto late = input;
  late[2].dividend_pv = late[1].dividend_pv + 0.5 * std::exp(-late[2].rate * 200.0 / 365.0);
  check(late, DividendScheduleReason::ExDateOutsideBracket, kValuationDay + 120, 1);
  // No PV left for a positive increment.
  auto empty = input;
  empty[2].dividend_pv = empty[1].dividend_pv * 0.9;
  check(empty, DividendScheduleReason::NonPositivePv, kValuationDay + 120, 1);
  // A zero rate carries no timing information.
  auto untimed = input;
  untimed[2].rate = 0.0;
  check(untimed, DividendScheduleReason::UntimedIncrement, kValuationDay + 120, 1);
  // A flat later expiry whose PV disagrees grossly with the schedule.
  auto repriced = ladder({{0.5, 12}}, {30, 60, 90});
  repriced[2].dividend_pv *= 1.01;
  check(repriced, DividendScheduleReason::RepricingMismatch, kValuationDay + 90, 1);
}

TEST(DividendSchedule, ExpiryEventsMapOntoTheExpirysOwnCarryClock) {
  const auto input = ladder({{0.5, 12}, {0.6, 30}, {0.7, 50}}, {20, 30, 60});
  const auto s = infer_dividend_schedule(kValuationDay, input);
  ASSERT_EQ(s.status, DividendScheduleStatus::Ok);
  std::array<CashDividend, 8> out{};
  const double years = 0.0841;
  const auto life = dividend_schedule_expiry_events(s, kValuationDay + 30, years, out);
  ASSERT_EQ(life.coverage, DividendExpiryCoverage::Covered);
  ASSERT_EQ(life.count, 2u); // the day-50 event is after this expiry
  EXPECT_DOUBLE_EQ(out[0].tau, years * 12.0 / 30.0);
  EXPECT_EQ(out[1].tau, years); // ex-date on the expiry date lands exactly at T
  EXPECT_DOUBLE_EQ(life.cash, 1.1);
  EXPECT_EQ(life.ambiguous, 0u);
  EXPECT_EQ(dividend_schedule_expiry_events(s, kValuationDay + 61, years, out).coverage,
            DividendExpiryCoverage::NotCovered);
  EXPECT_EQ(dividend_schedule_expiry_events(s, kValuationDay, years, out).coverage,
            DividendExpiryCoverage::InvalidInput);
  EXPECT_EQ(dividend_schedule_expiry_events(s, kValuationDay + 30, 0.0, out).coverage,
            DividendExpiryCoverage::InvalidInput);
  std::array<CashDividend, 1> tiny{};
  EXPECT_EQ(dividend_schedule_expiry_events(s, kValuationDay + 30, years, tiny).coverage,
            DividendExpiryCoverage::BufferTooSmall);
}

// The instants drive the shared escrow/lattice route; its window must admit
// exactly the events the clock mapping returns, with nothing outside its tau
// window, including the event ON the expiry date.
TEST(DividendSchedule, InstantsAgreeWithTheSharedDiscreteDividendRoute) {
  const auto input = ladder({{0.5, 12}, {0.6, 30}, {0.7, 50}}, {20, 30, 60});
  const auto s = infer_dividend_schedule(kValuationDay, input);
  std::array<DividendEvent, 8> instants{};
  const std::size_t n = dividend_schedule_instants(s, instants);
  ASSERT_EQ(n, 3u);
  std::array<DividendEvent, 2> small{};
  EXPECT_EQ(dividend_schedule_instants(s, small), 0u);
  const std::int64_t now = std::int64_t{kValuationDay} * kDividendScheduleNsPerDay;
  for (const int expiry : {11, 12, 29, 30, 60}) {
    const std::int64_t expiry_ns = now + std::int64_t{expiry} * kDividendScheduleNsPerDay;
    const double years = static_cast<double>(expiry_ns - now) / kCalendarYearNs;
    const auto route = discrete_div_route(std::span<const DividendEvent>(instants.data(), n),
                                          expiry_ns, now, years, 0.045,
                                          DiscreteDivPolicy::Lattice);
    std::array<CashDividend, 8> out{};
    const auto life = dividend_schedule_expiry_events(s, kValuationDay + expiry, 0.1, out);
    EXPECT_EQ(route.schedule.size(), life.count) << expiry;
    EXPECT_EQ(route.n_outside_tau_window, 0u) << expiry;
    EXPECT_EQ(route.applies(), life.count > 0) << expiry;
  }
}

TEST(DividendSchedule, InferenceIsDeterministic) {
  const auto input = ladder({{0.5, 20}, {0.5, 111}, {0.5, 202}, {0.5, 293}}, {30, 120, 300});
  const auto a = infer_dividend_schedule(kValuationDay, input);
  const auto b = infer_dividend_schedule(kValuationDay, input);
  ASSERT_EQ(a.n_events, b.n_events);
  for (std::size_t i = 0; i < a.n_events; ++i) {
    EXPECT_EQ(a.events[i].days, b.events[i].days);
    EXPECT_EQ(a.events[i].amount, b.events[i].amount);
    EXPECT_EQ(a.events[i].kind, b.events[i].kind);
  }
  EXPECT_EQ(a.max_pv_relative_residual, b.max_pv_relative_residual);
}

// A priced example through the module, end to end: a deep in-the-money American
// call on a stock paying 3.00 at day 20, valued for a 30-day expiry while the
// ex-date moves through and past the option's life.
TEST(DividendSchedulePricing, AmericanCallRespondsToTheExDateCrossingExpiry) {
  constexpr double kSpot = 100.0;
  constexpr double kStrike = 80.0;
  constexpr double kVol = 0.25;
  constexpr double kRate = 0.045;
  constexpr int kExpiry = 30;
  const double years = kExpiry / 365.0;
  const auto price = [&](int ex_day, Side side, ExerciseStyle style) {
    const auto s = infer_dividend_schedule(kValuationDay, ladder({{3.0, ex_day}}, {kExpiry, 90}));
    EXPECT_EQ(s.status, DividendScheduleStatus::Ok);
    std::array<CashDividend, 4> out{};
    const auto life = dividend_schedule_expiry_events(s, kValuationDay + kExpiry, years, out);
    EXPECT_EQ(life.coverage, DividendExpiryCoverage::Covered);
    const auto v = american_discrete_div_price(
        kSpot, kStrike, years, kVol, kRate, 0.0, side,
        std::span<const CashDividend>(out.data(), life.count), kDiscreteDivDefaultSteps, style);
    EXPECT_TRUE(v.has_value());
    return *v;
  };
  const auto no_div = american_discrete_div_price(kSpot, kStrike, years, kVol, kRate, 0.0,
                                                  Side::Call, {}, kDiscreteDivDefaultSteps);
  ASSERT_TRUE(no_div.has_value());
  const double c10 = price(10, Side::Call, ExerciseStyle::American);
  const double c20 = price(20, Side::Call, ExerciseStyle::American);
  const double c29 = price(29, Side::Call, ExerciseStyle::American);
  const double c30 = price(30, Side::Call, ExerciseStyle::American);
  const double c45 = price(45, Side::Call, ExerciseStyle::American);
  // Before expiry the cash lowers the call, but the holder's best response is to
  // exercise just before the ex-date: worth at least S - K e^{-r tau} (up to the
  // lattice placing the ex-step within one step of tau), so the dividend costs
  // only the interest on K after the ex-date, about K r (T - tau) = 0.10 here,
  // where the European call loses nearly the whole 3.00.
  EXPECT_LT(c20, *no_div - 0.05);
  EXPECT_GT(c20, price(20, Side::Call, ExerciseStyle::European) + 2.0);
  EXPECT_GE(c20, kSpot - kStrike * std::exp(-kRate * years * 20.0 / 30.0) - 1e-4);
  // A later ex-date leaves more interest on the strike: the value rises
  // monotonically toward the dividend-free value as the ex-date nears expiry.
  EXPECT_LT(c10, c20);
  EXPECT_LT(c20, c29);
  EXPECT_LE(c29, c30 + 1e-12);
  // On the expiry date the holder exercises cum-dividend at expiry, and past it
  // the cash is outside the life: both equal the dividend-free call exactly.
  EXPECT_EQ(c30, *no_div);
  EXPECT_EQ(c45, *no_div);
  // The put gains from cash paid inside its life and not from cash after it.
  const auto put_free = american_discrete_div_price(kSpot, kStrike, years, kVol, kRate, 0.0,
                                                    Side::Put, {}, kDiscreteDivDefaultSteps);
  ASSERT_TRUE(put_free.has_value());
  EXPECT_GT(price(20, Side::Put, ExerciseStyle::American), *put_free);
  EXPECT_EQ(price(45, Side::Put, ExerciseStyle::American), *put_free);
}

} // namespace
} // namespace atx::vol
