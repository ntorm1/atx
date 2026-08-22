#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "oracle_cohort_reader.hpp"
#include "oracle_dividends.hpp"

// Coverage for the dividend-schedule reconstructor (tools/oracle_dividends.*).
//
// The primary fixture is REAL: it is the SPY expiry ladder the reference
// measurement ran on, carried here as the (years, ddiv) pairs the store held.
// Each of the eight jumps and each of the six bracketing expiries is a value
// that came off the parquet, not a value invented for a test.

namespace {

using atx::vol::CashDividend;
using atx::vol::ErrorCode;
using atx::vol::oracle::accrued_dividend;
using atx::vol::oracle::DividendCadence;
using atx::vol::oracle::DividendRefusal;
using atx::vol::oracle::DividendReconstruction;
using atx::vol::oracle::OracleRow;
using atx::vol::oracle::reconstruct_dividends;
using atx::vol::oracle::split_merged_dividends;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] OracleRow make_row(std::string_view date, std::string_view underlier, double years,
                                 double ddiv) {
  OracleRow row;
  row.date = std::string{date};
  row.underlier = std::string{underlier};
  row.bucket_et = "1600";
  row.years = years;
  row.ddiv = ddiv;
  return row;
}

// The SPY ladder: every distinct `years` that brackets a dividend jump, paired
// with the `ddiv` the store carried at that expiry.
struct Rung {
  double years;
  double ddiv;
};

constexpr Rung kSpyLadder[] = {
    {0.0771711, 0.00},  {0.0967677, 1.98}, {0.294653, 1.98}, {0.347905, 4.13},
    {0.457973, 4.13},   {0.592603, 6.08},  {0.622262, 6.08}, {0.838835, 8.13},
    {0.872009, 8.13},   {1.08959, 10.28},  {1.34073, 12.63}, {1.43371, 12.63},
    {1.83768, 16.63},   {2.3349, 21.13},
};

// The eight dividends that ladder must difference back out to. Amounts and
// upper-bracket ex-dates both come from the reference reconstruction.
constexpr Rung kSpySchedule[] = {
    {0.0967677, 1.98}, {0.347905, 2.15}, {0.592603, 1.95}, {0.838835, 2.05},
    {1.08959, 2.15},   {1.34073, 2.35},  {1.83768, 4.00},  {2.3349, 4.50},
};

[[nodiscard]] std::vector<OracleRow> spy_rows() {
  std::vector<OracleRow> rows;
  for (const Rung &rung : kSpyLadder) {
    // Several strikes per expiry, exactly as the store holds them: the
    // collapse-to-one-point-per-expiry step has to be exercised, not assumed.
    for (int strike = 0; strike < 3; ++strike) {
      rows.push_back(make_row("2026-08-14", "SPY", rung.years, rung.ddiv));
    }
  }
  return rows;
}

[[nodiscard]] std::vector<CashDividend> reconstructed_spy_schedule() {
  const DividendReconstruction out = reconstruct_dividends(spy_rows());
  EXPECT_EQ(out.schedules.size(), 1U);
  return out.schedules.empty() ? std::vector<CashDividend>{} : out.schedules.front().dividends;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Reconstruction
// ═══════════════════════════════════════════════════════════════════════════

TEST(OracleDividends, RecoversTheSpyScheduleFromTheDdivStepFunction) {
  const DividendReconstruction out = reconstruct_dividends(spy_rows());
  EXPECT_EQ(out.rows_seen, 42U);
  EXPECT_EQ(out.groups_seen, 1U);
  EXPECT_EQ(out.refusals.total(), 0U);
  EXPECT_TRUE(out.refused.empty());
  ASSERT_EQ(out.schedules.size(), 1U);

  const auto &schedule = out.schedules.front();
  EXPECT_EQ(schedule.key.date, "2026-08-14");
  EXPECT_EQ(schedule.key.underlier, "SPY");
  EXPECT_EQ(schedule.rows, 42U);
  EXPECT_EQ(schedule.expiries, std::size(kSpyLadder));
  ASSERT_EQ(schedule.dividends.size(), std::size(kSpySchedule));

  for (std::size_t i = 0; i < schedule.dividends.size(); ++i) {
    // The ex-date is the `years` of the FIRST expiry that included the
    // dividend, taken verbatim off the row — hence exact, not near.
    EXPECT_DOUBLE_EQ(schedule.dividends[i].tau, kSpySchedule[i].years) << "dividend " << i;
    EXPECT_NEAR(schedule.dividends[i].amount, kSpySchedule[i].ddiv, 1.0e-12) << "dividend " << i;
  }
}

// The invariant the whole reconstruction exists to satisfy. The reference
// measured 1.78e-15 across all 14,357 SPY rows; the bar here is 1e-12 and the
// worst observed residual is reported so a drift toward it is visible.
TEST(OracleDividends, AccrualInvariantHoldsForEveryRow) {
  const std::vector<CashDividend> schedule = reconstructed_spy_schedule();
  ASSERT_FALSE(schedule.empty());
  double worst = 0.0;
  for (const OracleRow &row : spy_rows()) {
    const double accrued = accrued_dividend(schedule, row.years);
    worst = std::max(worst, std::abs(accrued - row.ddiv));
  }
  EXPECT_LT(worst, 1.0e-12) << "max |sum(D_i : tau_i <= years) - ddiv| = " << worst;
}

TEST(OracleDividends, NoDividendsIsDistinguishableFromARefusal) {
  std::vector<OracleRow> rows;
  for (const double years : {0.1, 0.3, 0.7, 1.5}) {
    rows.push_back(make_row("2026-08-14", "AAPL", years, 0.0));
  }
  const DividendReconstruction out = reconstruct_dividends(rows);
  ASSERT_EQ(out.schedules.size(), 1U);
  EXPECT_TRUE(out.schedules.front().dividends.empty());
  EXPECT_EQ(out.schedules.front().expiries, 4U);
  // THE distinguishing fact: an empty schedule with zero refusals means "pays
  // nothing", and a caller can see that without inspecting the rows again.
  EXPECT_EQ(out.refusals.total(), 0U);
  EXPECT_TRUE(out.refused.empty());
}

TEST(OracleDividends, DividendBeforeTheEarliestExpiry_IsEmittedAtThatExpiry) {
  // `ddiv` at the earliest expiry is ALREADY a sum, so a positive value there
  // is a real dividend whose upper bracket is that first expiry. Differencing
  // from a 0 baseline is what recovers it; differencing from the first row
  // would silently drop it.
  const std::vector<OracleRow> rows{
      make_row("2026-08-14", "SPY", 0.10, 1.25),
      make_row("2026-08-14", "SPY", 0.40, 1.25),
      make_row("2026-08-14", "SPY", 0.60, 2.50),
  };
  const DividendReconstruction out = reconstruct_dividends(rows);
  ASSERT_EQ(out.schedules.size(), 1U);
  ASSERT_EQ(out.schedules.front().dividends.size(), 2U);
  EXPECT_DOUBLE_EQ(out.schedules.front().dividends[0].tau, 0.10);
  EXPECT_NEAR(out.schedules.front().dividends[0].amount, 1.25, 1.0e-12);
  EXPECT_DOUBLE_EQ(out.schedules.front().dividends[1].tau, 0.60);
  EXPECT_NEAR(out.schedules.front().dividends[1].amount, 1.25, 1.0e-12);
}

TEST(OracleDividends, GroupsBySnapshotDateAndUnderlier) {
  std::vector<OracleRow> rows{
      make_row("2026-08-14", "SPY", 0.10, 0.00), make_row("2026-08-14", "SPY", 0.40, 1.98),
      make_row("2026-08-15", "SPY", 0.10, 0.00), make_row("2026-08-15", "SPY", 0.40, 2.10),
      make_row("2026-08-14", "QQQ", 0.10, 0.00), make_row("2026-08-14", "QQQ", 0.40, 0.75),
  };
  const DividendReconstruction out = reconstruct_dividends(rows);
  EXPECT_EQ(out.groups_seen, 3U);
  ASSERT_EQ(out.schedules.size(), 3U);
  // Two dates of the same name are two independent schedules, not one merged
  // one — a snapshot's `ddiv` is a property of that snapshot.
  for (const auto &schedule : out.schedules) {
    ASSERT_EQ(schedule.dividends.size(), 1U);
    EXPECT_DOUBLE_EQ(schedule.dividends.front().tau, 0.40);
  }
}

TEST(OracleDividends, RowOrderDoesNotMatter) {
  std::vector<OracleRow> shuffled = spy_rows();
  std::reverse(shuffled.begin(), shuffled.end());
  const DividendReconstruction forward = reconstruct_dividends(spy_rows());
  const DividendReconstruction reversed = reconstruct_dividends(shuffled);
  ASSERT_EQ(forward.schedules.size(), 1U);
  ASSERT_EQ(reversed.schedules.size(), 1U);
  const auto &a = forward.schedules.front().dividends;
  const auto &b = reversed.schedules.front().dividends;
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_DOUBLE_EQ(a[i].tau, b[i].tau);
    EXPECT_DOUBLE_EQ(a[i].amount, b[i].amount);
  }
}

TEST(OracleDividends, EmptyInputYieldsNothingAndRefusesNothing) {
  const DividendReconstruction out = reconstruct_dividends({});
  EXPECT_EQ(out.rows_seen, 0U);
  EXPECT_EQ(out.groups_seen, 0U);
  EXPECT_TRUE(out.schedules.empty());
  EXPECT_EQ(out.refusals.total(), 0U);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Fail closed — one test per malformed shape, each counted by its own reason
// ═══════════════════════════════════════════════════════════════════════════

TEST(OracleDividends, RefusesNonMonotoneDdiv) {
  const std::vector<OracleRow> rows{
      make_row("2026-08-14", "SPY", 0.10, 0.00),
      make_row("2026-08-14", "SPY", 0.40, 1.98),
      make_row("2026-08-14", "SPY", 0.70, 1.50), // a cumulative sum cannot shrink
  };
  const DividendReconstruction out = reconstruct_dividends(rows);
  EXPECT_TRUE(out.schedules.empty()) << "a refused group must contribute NO schedule";
  ASSERT_EQ(out.refused.size(), 1U);
  EXPECT_EQ(out.refused.front().reason, DividendRefusal::NonMonotoneDdiv);
  EXPECT_DOUBLE_EQ(out.refused.front().years, 0.70);
  EXPECT_EQ(out.refusals.non_monotone_ddiv, 1U);
  EXPECT_EQ(out.refusals.total(), 1U);
}

TEST(OracleDividends, RefusesANegativeDdivAtTheEarliestExpiry) {
  const std::vector<OracleRow> rows{
      make_row("2026-08-14", "SPY", 0.10, -0.50),
      make_row("2026-08-14", "SPY", 0.40, 1.98),
  };
  const DividendReconstruction out = reconstruct_dividends(rows);
  EXPECT_TRUE(out.schedules.empty());
  ASSERT_EQ(out.refused.size(), 1U);
  EXPECT_EQ(out.refused.front().reason, DividendRefusal::NonMonotoneDdiv);
  EXPECT_EQ(out.refusals.non_monotone_ddiv, 1U);
}

TEST(OracleDividends, RefusesTwoDistinctDdivAtOneExpiry) {
  const std::vector<OracleRow> rows{
      make_row("2026-08-14", "SPY", 0.10, 0.00),
      make_row("2026-08-14", "SPY", 0.40, 1.98),
      make_row("2026-08-14", "SPY", 0.40, 2.15), // same expiry, two answers
  };
  const DividendReconstruction out = reconstruct_dividends(rows);
  EXPECT_TRUE(out.schedules.empty());
  ASSERT_EQ(out.refused.size(), 1U);
  EXPECT_EQ(out.refused.front().reason, DividendRefusal::AmbiguousDdivAtExpiry);
  EXPECT_DOUBLE_EQ(out.refused.front().years, 0.40);
  EXPECT_EQ(out.refusals.ambiguous_ddiv_at_expiry, 1U);
}

TEST(OracleDividends, RefusesAChangeTooSmallToBeCash) {
  // 1e-10: above the flat tolerance, so it is a real change in the column, but
  // three orders of magnitude below anything that could be a cash dividend.
  const std::vector<OracleRow> rows{
      make_row("2026-08-14", "SPY", 0.10, 0.00),
      make_row("2026-08-14", "SPY", 0.40, 1.0e-10),
  };
  const DividendReconstruction out = reconstruct_dividends(rows);
  EXPECT_TRUE(out.schedules.empty());
  ASSERT_EQ(out.refused.size(), 1U);
  EXPECT_EQ(out.refused.front().reason, DividendRefusal::NonPositiveJump);
  EXPECT_EQ(out.refusals.non_positive_jump, 1U);
}

TEST(OracleDividends, ColumnNoiseBelowTheFlatToleranceIsNotADividend) {
  // The mirror image of the test above: a 1e-15 wobble is the telescoping
  // residual a float column carries, and refusing on it would refuse every
  // real underlier.
  const std::vector<OracleRow> rows{
      make_row("2026-08-14", "SPY", 0.10, 1.98),
      make_row("2026-08-14", "SPY", 0.40, 1.98 + 1.0e-15),
      make_row("2026-08-14", "SPY", 0.70, 1.98 - 1.0e-15),
  };
  const DividendReconstruction out = reconstruct_dividends(rows);
  EXPECT_EQ(out.refusals.total(), 0U);
  ASSERT_EQ(out.schedules.size(), 1U);
  ASSERT_EQ(out.schedules.front().dividends.size(), 1U);
  EXPECT_DOUBLE_EQ(out.schedules.front().dividends.front().tau, 0.10);
}

TEST(OracleDividends, RefusesNonFiniteInput) {
  for (const auto &pair : {std::pair<double, double>{kNaN, 1.98},
                           std::pair<double, double>{0.40, kNaN},
                           std::pair<double, double>{
                               std::numeric_limits<double>::infinity(), 1.98}}) {
    const std::vector<OracleRow> rows{
        make_row("2026-08-14", "SPY", 0.10, 0.00),
        make_row("2026-08-14", "SPY", pair.first, pair.second),
    };
    const DividendReconstruction out = reconstruct_dividends(rows);
    EXPECT_TRUE(out.schedules.empty());
    ASSERT_EQ(out.refused.size(), 1U);
    EXPECT_EQ(out.refused.front().reason, DividendRefusal::NonFiniteInput);
    EXPECT_EQ(out.refusals.non_finite_input, 1U);
  }
}

// A refusal is per GROUP. One bad underlier must not cost the good ones their
// schedules, and the counts must say how many were lost and why.
TEST(OracleDividends, RefusalsAreIsolatedToTheirGroupAndCountedByReason) {
  std::vector<OracleRow> rows;
  for (const Rung &rung : kSpyLadder) {
    rows.push_back(make_row("2026-08-14", "SPY", rung.years, rung.ddiv));
  }
  rows.push_back(make_row("2026-08-14", "BAD1", 0.10, 1.00));
  rows.push_back(make_row("2026-08-14", "BAD1", 0.40, 0.50)); // non-monotone
  rows.push_back(make_row("2026-08-14", "BAD2", 0.10, 1.00));
  rows.push_back(make_row("2026-08-14", "BAD2", 0.10, 2.00)); // ambiguous
  rows.push_back(make_row("2026-08-14", "GOOD", 0.10, 0.00));
  rows.push_back(make_row("2026-08-14", "GOOD", 0.40, 0.88));

  const DividendReconstruction out = reconstruct_dividends(rows);
  EXPECT_EQ(out.groups_seen, 4U);
  EXPECT_EQ(out.schedules.size(), 2U);
  EXPECT_EQ(out.refused.size(), 2U);
  EXPECT_EQ(out.refusals.non_monotone_ddiv, 1U);
  EXPECT_EQ(out.refusals.ambiguous_ddiv_at_expiry, 1U);
  EXPECT_EQ(out.refusals.total(), 2U);

  const auto spy = std::find_if(out.schedules.begin(), out.schedules.end(),
                                [](const auto &s) { return s.key.underlier == "SPY"; });
  ASSERT_NE(spy, out.schedules.end());
  EXPECT_EQ(spy->dividends.size(), std::size(kSpySchedule));
}

TEST(OracleDividends, RefusalReasonsHaveDistinctNames) {
  EXPECT_EQ(atx::vol::oracle::to_string(DividendRefusal::NonFiniteInput), "NonFiniteInput");
  EXPECT_EQ(atx::vol::oracle::to_string(DividendRefusal::AmbiguousDdivAtExpiry),
            "AmbiguousDdivAtExpiry");
  EXPECT_EQ(atx::vol::oracle::to_string(DividendRefusal::NonMonotoneDdiv), "NonMonotoneDdiv");
  EXPECT_EQ(atx::vol::oracle::to_string(DividendRefusal::NonPositiveJump), "NonPositiveJump");
}

// ═══════════════════════════════════════════════════════════════════════════
//  The OPT-IN merged-jump split
// ═══════════════════════════════════════════════════════════════════════════

TEST(OracleDividends, SplitIsOptInAndReconstructionNeverDoesIt) {
  // The core reconstructor knows nothing about cadence: SPY's 2028 jumps come
  // back merged, at 4.00 and 4.50, exactly as the expiry ladder recorded them.
  const std::vector<CashDividend> schedule = reconstructed_spy_schedule();
  ASSERT_EQ(schedule.size(), 8U);
  EXPECT_NEAR(schedule[6].amount, 4.00, 1.0e-12);
  EXPECT_NEAR(schedule[7].amount, 4.50, 1.0e-12);
}

TEST(OracleDividends, SplitsTheMergedSpyQuartersOntoTheQuarterlyCadence) {
  const std::vector<CashDividend> schedule = reconstructed_spy_schedule();
  ASSERT_EQ(schedule.size(), 8U);
  const auto split = split_merged_dividends(schedule, DividendCadence{});
  ASSERT_TRUE(split.has_value()) << split.error().to_string();
  ASSERT_EQ(split->size(), 10U) << "the two merged quarters become four";

  // The six quarterly dividends pass through untouched.
  for (std::size_t i = 0; i < 6U; ++i) {
    EXPECT_DOUBLE_EQ((*split)[i].tau, schedule[i].tau) << "dividend " << i;
    EXPECT_DOUBLE_EQ((*split)[i].amount, schedule[i].amount) << "dividend " << i;
  }

  // The reference calendar rule put SPY's split ex-dates on the 3rd Fridays of
  // 2028-03-17 and 2028-09-15, which the store's year-fraction convention maps
  // to 1.587900 and 2.085027. The cadence rule lands within 2.2e-4 years of
  // both, four orders of magnitude below the tau placement error the price
  // measurement is sensitive to — and it needs no calendar to do it.
  EXPECT_NEAR((*split)[6].tau, 1.587900, 5.0e-4);
  EXPECT_NEAR((*split)[6].amount, 2.00, 1.0e-12);
  EXPECT_DOUBLE_EQ((*split)[7].tau, 1.83768) << "the last instalment keeps the upper bracket";
  EXPECT_NEAR((*split)[7].amount, 2.00, 1.0e-12);

  EXPECT_NEAR((*split)[8].tau, 2.085027, 5.0e-4);
  EXPECT_NEAR((*split)[8].amount, 2.25, 1.0e-12);
  EXPECT_DOUBLE_EQ((*split)[9].tau, 2.3349);
  EXPECT_NEAR((*split)[9].amount, 2.25, 1.0e-12);

  // Ascending, and every amount still a real dividend.
  for (std::size_t i = 1; i < split->size(); ++i) {
    EXPECT_GT((*split)[i].tau, (*split)[i - 1U].tau);
    EXPECT_GT((*split)[i].amount, 0.0);
  }
}

TEST(OracleDividends, SplitPreservesTheTotalAndTheFarDatedAccrual) {
  const std::vector<CashDividend> schedule = reconstructed_spy_schedule();
  const auto split = split_merged_dividends(schedule, DividendCadence{});
  ASSERT_TRUE(split.has_value()) << split.error().to_string();
  // Splitting REDISTRIBUTES an amount in time; it never creates or destroys
  // cash, so the accrual at the last expiry is unchanged.
  EXPECT_NEAR(accrued_dividend(*split, 10.0), accrued_dividend(schedule, 10.0), 1.0e-12);
  // And every expiry at or before the first merge still accrues identically.
  for (const Rung &rung : kSpyLadder) {
    if (rung.years > 1.34073) {
      continue;
    }
    EXPECT_NEAR(accrued_dividend(*split, rung.years), accrued_dividend(schedule, rung.years),
                1.0e-12)
        << "years=" << rung.years;
  }
}

TEST(OracleDividends, SplitIsANoOpOnAScheduleThatIsAlreadyOnCadence) {
  const std::vector<CashDividend> quarterly{
      {0.0967677, 1.98}, {0.347905, 2.15}, {0.592603, 1.95},
      {0.838835, 2.05},  {1.08959, 2.15},  {1.34073, 2.35}};
  const auto split = split_merged_dividends(quarterly, DividendCadence{});
  ASSERT_TRUE(split.has_value()) << split.error().to_string();
  ASSERT_EQ(split->size(), quarterly.size());
  for (std::size_t i = 0; i < quarterly.size(); ++i) {
    EXPECT_DOUBLE_EQ((*split)[i].tau, quarterly[i].tau);
    EXPECT_DOUBLE_EQ((*split)[i].amount, quarterly[i].amount);
  }
}

TEST(OracleDividends, SplitRefusesAGapWiderThanTheCadenceAllows) {
  const std::vector<CashDividend> annual{{0.25, 1.0}, {2.75, 4.0}}; // 10 quarters apart
  const auto split = split_merged_dividends(annual, DividendCadence{});
  ASSERT_FALSE(split.has_value());
  EXPECT_EQ(split.error().code(), ErrorCode::OutOfRange);
}

TEST(OracleDividends, SplitRefusesAMalformedCadence) {
  const std::vector<CashDividend> schedule = reconstructed_spy_schedule();
  for (const DividendCadence &bad : {DividendCadence{0.0, 4}, DividendCadence{-0.25, 4},
                                     DividendCadence{kNaN, 4}, DividendCadence{0.25, 0}}) {
    const auto split = split_merged_dividends(schedule, bad);
    ASSERT_FALSE(split.has_value());
    EXPECT_EQ(split.error().code(), ErrorCode::InvalidArgument);
  }
}

TEST(OracleDividends, SplitRefusesAMalformedSchedule) {
  const std::vector<std::vector<CashDividend>> malformed{
      {{0.50, 1.0}, {0.25, 1.0}},  // not ascending
      {{0.25, 1.0}, {0.25, 1.0}},  // duplicate tau
      {{0.25, 0.0}},               // amount not positive
      {{0.25, -1.0}},              // negative amount
      {{-0.25, 1.0}},              // ex-date in the past
      {{kNaN, 1.0}},               // non-finite tau
      {{0.25, kNaN}},              // non-finite amount
  };
  for (const std::vector<CashDividend> &schedule : malformed) {
    const auto split = split_merged_dividends(schedule, DividendCadence{});
    ASSERT_FALSE(split.has_value());
    EXPECT_EQ(split.error().code(), ErrorCode::InvalidArgument);
  }
}

TEST(OracleDividends, SplitOfAnEmptyScheduleIsEmpty) {
  const auto split = split_merged_dividends({}, DividendCadence{});
  ASSERT_TRUE(split.has_value()) << split.error().to_string();
  EXPECT_TRUE(split->empty());
}

TEST(OracleDividends, TheFirstDividendIsNeverSplit) {
  // With no previous ex-date there is no gap to read a part count from, so a
  // merge at the front of a schedule is undetectable — and guessing there would
  // be a fabrication, not a reconstruction.
  const std::vector<CashDividend> merged_front{{0.75, 6.0}, {1.00, 2.0}};
  const auto split = split_merged_dividends(merged_front, DividendCadence{});
  ASSERT_TRUE(split.has_value()) << split.error().to_string();
  ASSERT_EQ(split->size(), 2U);
  EXPECT_DOUBLE_EQ((*split)[0].tau, 0.75);
  EXPECT_DOUBLE_EQ((*split)[0].amount, 6.0);
}

// ═══════════════════════════════════════════════════════════════════════════
//  The reconstructor feeds the pricer directly
// ═══════════════════════════════════════════════════════════════════════════

TEST(OracleDividends, ReconstructedScheduleFeedsTheLatticeAndTheSplitMovesThePrice) {
  const std::vector<CashDividend> merged = reconstructed_spy_schedule();
  const auto split = split_merged_dividends(merged, DividendCadence{});
  ASSERT_TRUE(split.has_value()) << split.error().to_string();

  // A 2028-06-16 SPY option: both 2028 quarters are inside its life, and
  // splitting moves 2.00 of cash from the expiry back one quarter.
  constexpr double kT = 1.83768;
  const auto with_merged = atx::vol::american_discrete_div_price(
      775.8, 700.0, kT, 0.18, 0.041, 0.008, atx::vol::Side::Put, merged, 301,
      atx::vol::ExerciseStyle::American);
  ASSERT_TRUE(with_merged.has_value()) << with_merged.error().to_string();
  const auto with_split = atx::vol::american_discrete_div_price(
      775.8, 700.0, kT, 0.18, 0.041, 0.008, atx::vol::Side::Put, *split, 301,
      atx::vol::ExerciseStyle::American);
  ASSERT_TRUE(with_split.has_value()) << with_split.error().to_string();

  EXPECT_TRUE(std::isfinite(*with_merged));
  EXPECT_TRUE(std::isfinite(*with_split));
  EXPECT_GT(*with_merged, 0.0);
  EXPECT_GT(*with_split, 0.0);
  // The same total cash on different dates is a different price — which is the
  // whole reason the split is worth doing. The DIRECTION is a modelling
  // consequence, not a property this test pins.
  EXPECT_NE(*with_merged, *with_split);
}

} // namespace
