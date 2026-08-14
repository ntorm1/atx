// Davis-Hobson quote feasibility (detail/quote_feasibility.hpp), Thm 3.1.
//
// The fixtures are built in NORMALISED coordinates and then pushed back into
// currency units, so each case says out loud which of the three conditions it
// is aimed at. Forward and discount are deliberately non-trivial (F = 100,
// DF = 0.99) in most cases so a dropped normalisation cannot pass.

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/detail/quote_feasibility.hpp"

namespace {

using atx::vol::detail::CallQuote;
using atx::vol::detail::check_quote_feasibility;
using atx::vol::detail::feasibility_scratch_size;
using atx::vol::detail::FeasibilityFailure;
using atx::vol::detail::FeasibilityInputs;
using atx::vol::detail::FeasibilityNode;
using atx::vol::detail::FeasibilityReport;
using atx::vol::detail::FeasibilityVerdict;
using atx::vol::detail::kNoQuoteIndex;

constexpr double kF = 100.0;
constexpr double kDf = 0.99;

// (x, r) in normalised coordinates -> a currency-unit two-sided quote.
[[nodiscard]] CallQuote norm_quote(double x, double r, double forward = kF, double discount = kDf) {
  return CallQuote{x * forward, r * discount * forward, true, true};
}

[[nodiscard]] FeasibilityInputs base_inputs() {
  FeasibilityInputs in;
  in.forward = kF;
  in.discount = kDf;
  return in;
}

// Sized-to-fit scratch; the helper exists so no test hand-rolls the size rule.
class Harness {
public:
  [[nodiscard]] FeasibilityReport run(const std::vector<CallQuote> &quotes,
                                      const FeasibilityInputs &in) {
    scratch_.assign(feasibility_scratch_size(quotes.size()), FeasibilityNode{});
    weak_.assign(quotes.size() + 1u, kNoQuoteIndex);
    return check_quote_feasibility(std::span<const CallQuote>{quotes}, in,
                                   std::span<FeasibilityNode>{scratch_},
                                   std::span<std::uint32_t>{weak_});
  }
  [[nodiscard]] const std::vector<std::uint32_t> &weak() const noexcept { return weak_; }

private:
  std::vector<FeasibilityNode> scratch_;
  std::vector<std::uint32_t> weak_;
};

// A textbook-clean strictly convex, strictly decreasing set that satisfies all
// three conditions with room to spare. r(x) = max(1-x, 0) + cushion.
[[nodiscard]] std::vector<CallQuote> clean_slice() {
  return {
      norm_quote(0.80, 0.2200), norm_quote(0.90, 0.1400), norm_quote(1.00, 0.0800),
      norm_quote(1.10, 0.0400), norm_quote(1.20, 0.0175),
  };
}

// ---------------------------------------------------------------- feasible --

TEST(QuoteFeasibility, CleanConvexSlice_AllThreeConditionsHold_Feasible) {
  Harness h;
  const FeasibilityReport rep = h.run(clean_slice(), base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::Feasible);
  EXPECT_EQ(rep.failure, FeasibilityFailure::None);
  EXPECT_EQ(rep.n_points, 5u);
  EXPECT_EQ(rep.n_weak_points, 0u);
  EXPECT_EQ(rep.first_failure_index, kNoQuoteIndex);
  EXPECT_GT(rep.min_drop, 0.0);
  EXPECT_GE(rep.slope_at_zero, -1.0);
  EXPECT_LE(rep.max_excess, 1e-9);
}

TEST(QuoteFeasibility, ExactlyOnMinorant_CollinearInteriorPoint_Feasible) {
  // Three collinear points: the middle one is ON its minorant, not above it,
  // so condition 3 holds even though it is not a hull VERTEX.
  Harness h;
  const std::vector<CallQuote> q{
      norm_quote(0.50, 0.5000),
      norm_quote(0.75, 0.2500),
      norm_quote(1.00, 0.0000),
      norm_quote(1.50, 0.0000),
  };
  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::Feasible) << to_string(rep.failure);
  EXPECT_EQ(rep.n_weak_points, 0u);
}

TEST(QuoteFeasibility, AllCallsWorthless_LowestStrikeAboveForward_Feasible) {
  // Every call is worth zero. n0 = 1, so condition 1 only has to hold on
  // [0, x_1]; the single segment (0,1) -> (1.2, 0) has slope -1/1.2 >= -1.
  Harness h;
  const std::vector<CallQuote> q{
      norm_quote(1.20, 0.0),
      norm_quote(1.40, 0.0),
      norm_quote(1.60, 0.0),
  };
  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::Feasible) << to_string(rep.failure);
  EXPECT_NEAR(rep.slope_at_zero, -1.0 / 1.20, 1e-12);
}

TEST(QuoteFeasibility, ZeroPriceTolerance_QuantisedWingFlat_Feasible) {
  // Real OPRA wings: a run of strikes all mid-quoting the same half-tick. With
  // the textbook zero_price_tol = 0 that flat run is a condition-1 failure;
  // with a half-tick threshold it is correctly read as "worthless".
  Harness h;
  const std::vector<CallQuote> q{
      CallQuote{100.0, 8.00, true, true},  CallQuote{110.0, 4.00, true, true},
      CallQuote{120.0, 0.025, true, true}, CallQuote{130.0, 0.025, true, true},
      CallQuote{140.0, 0.025, true, true},
  };

  FeasibilityInputs strict = base_inputs();
  const FeasibilityReport rep_strict = h.run(q, strict);
  EXPECT_EQ(rep_strict.verdict, FeasibilityVerdict::ModelIndependentArbitrage);
  EXPECT_EQ(rep_strict.failure, FeasibilityFailure::NotStrictlyDecreasing);

  FeasibilityInputs tol = base_inputs();
  tol.zero_price_tol = 0.03 / (kDf * kF); // a hair above the 2.5-cent wing mid
  const FeasibilityReport rep_tol = h.run(q, tol);
  EXPECT_EQ(rep_tol.verdict, FeasibilityVerdict::Feasible) << to_string(rep_tol.failure);
}

// -------------------------------------------------- weak arbitrage (cond 3) --

TEST(QuoteFeasibility, ButterflyViolation_PointAboveMinorant_WeakArbitrage) {
  // Strictly decreasing prices, but the middle strike is concave-up: it sits
  // above the chord joining its neighbours, so R(x_2) < r_2.
  Harness h;
  std::vector<CallQuote> q = clean_slice();
  q[2].price = 0.1300 * kDf * kF; // was 0.0800; now above the 0.90/1.10 chord

  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::WeakArbitrage);
  EXPECT_EQ(rep.failure, FeasibilityFailure::AboveConvexMinorant);
  EXPECT_EQ(rep.n_weak_points, 1u);
  EXPECT_EQ(rep.n_weak_reported, 1u);
  EXPECT_EQ(rep.first_failure_index, 2u);
  EXPECT_EQ(h.weak()[0], 2u);
  EXPECT_GT(rep.max_excess, 1e-6);
  // Conditions 1 and 2 still hold, which is what makes it WEAK and not MIA.
  EXPECT_GT(rep.min_drop, 0.0);
  EXPECT_GE(rep.slope_at_zero, -1.0);
}

TEST(QuoteFeasibility, TwoButterflyViolations_BothStrikesReported) {
  Harness h;
  std::vector<CallQuote> q = clean_slice();
  q[1].price = 0.1900 * kDf * kF;
  q[3].price = 0.0700 * kDf * kF;

  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::WeakArbitrage);
  EXPECT_EQ(rep.n_weak_points, 2u);
  EXPECT_EQ(rep.n_weak_reported, 2u);
  EXPECT_EQ(rep.first_failure_index, 1u);
  EXPECT_EQ(h.weak()[0], 1u);
  EXPECT_EQ(h.weak()[1], 3u);
}

TEST(QuoteFeasibility, WeakOutTooShort_CountStillTrue_ReportedTruncated) {
  std::vector<CallQuote> q = clean_slice();
  q[1].price = 0.1900 * kDf * kF;
  q[3].price = 0.0700 * kDf * kF;

  std::vector<FeasibilityNode> scratch(feasibility_scratch_size(q.size()));
  std::array<std::uint32_t, 1> sink{kNoQuoteIndex};
  const FeasibilityReport rep =
      check_quote_feasibility(std::span<const CallQuote>{q}, base_inputs(),
                              std::span<FeasibilityNode>{scratch}, std::span<std::uint32_t>{sink});

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::WeakArbitrage);
  EXPECT_EQ(rep.n_weak_points, 2u);
  EXPECT_EQ(rep.n_weak_reported, 1u);
  EXPECT_EQ(sink[0], 1u);
}

TEST(QuoteFeasibility, NoWeakSink_CountAndFirstIndexStillReported) {
  std::vector<CallQuote> q = clean_slice();
  q[2].price = 0.1300 * kDf * kF;

  std::vector<FeasibilityNode> scratch(feasibility_scratch_size(q.size()));
  const FeasibilityReport rep = check_quote_feasibility(
      std::span<const CallQuote>{q}, base_inputs(), std::span<FeasibilityNode>{scratch});

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::WeakArbitrage);
  EXPECT_EQ(rep.n_weak_points, 1u);
  EXPECT_EQ(rep.n_weak_reported, 0u);
  EXPECT_EQ(rep.first_failure_index, 2u);
}

// ------------------------------- model-independent arbitrage (cond 1 and 2) --

TEST(QuoteFeasibility, CallSpreadInversionAtWing_FlatMinorant_ModelIndependent) {
  // The highest strike is quoted RICHER than the one below it, and nothing
  // cheaper lies beyond to pull the hull down. The minorant therefore goes
  // flat, so condition 1 AND condition 3 both fail; the documented precedence
  // reports the stronger verdict, and the butterfly count survives it.
  Harness h;
  const std::vector<CallQuote> q{
      norm_quote(0.90, 0.1400),
      norm_quote(1.00, 0.0800),
      norm_quote(1.10, 0.1000),
  };
  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::ModelIndependentArbitrage);
  EXPECT_EQ(rep.failure, FeasibilityFailure::NotStrictlyDecreasing);
  EXPECT_EQ(rep.first_failure_index, 2u);
  EXPECT_NEAR(rep.min_drop, 0.0, 1e-12);
  EXPECT_EQ(rep.n_weak_points, 1u);
  EXPECT_EQ(h.weak()[0], 2u);
}

TEST(QuoteFeasibility, CallSpreadInversionInInterior_MinorantAbsorbsIt_WeakArbitrage) {
  // The SAME inversion with a cheaper strike beyond it: the hull skips the
  // inverted point entirely, so R stays strictly decreasing and only condition
  // 3 fails. Documented behaviour, not a near-miss -- the taxonomy is about the
  // MINORANT's shape, not about the raw quote sequence.
  Harness h;
  std::vector<CallQuote> q = clean_slice();
  q[3].price = 0.1000 * kDf * kF; // x=1.10 richer than x=1.00's 0.08

  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::WeakArbitrage);
  EXPECT_EQ(rep.failure, FeasibilityFailure::AboveConvexMinorant);
  EXPECT_EQ(rep.first_failure_index, 3u);
  EXPECT_GT(rep.min_drop, 0.0);
}

TEST(QuoteFeasibility, FlatCallSpreadAtRealValue_NotStrictlyDecreasing) {
  // Two adjacent wing strikes at an identical, materially non-zero price: a
  // costless call spread with a payoff that is positive with positive
  // probability. zero_price_tol is 0, so these are not "worthless".
  Harness h;
  const std::vector<CallQuote> q{
      norm_quote(0.90, 0.1400),
      norm_quote(1.00, 0.0800),
      norm_quote(1.10, 0.0800),
  };
  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::ModelIndependentArbitrage);
  EXPECT_EQ(rep.failure, FeasibilityFailure::NotStrictlyDecreasing);
  EXPECT_EQ(rep.first_failure_index, 2u);
  EXPECT_NEAR(rep.min_drop, 0.0, 1e-12);
}

TEST(QuoteFeasibility, BelowIntrinsic_SlopeAtZeroBelowMinusOne_ModelIndependent) {
  // r_1 = 0.10 at x_1 = 0.50 needs r_1 >= 1 - x_1 = 0.50. R'(0+) = -1.8.
  Harness h;
  const std::vector<CallQuote> q{
      norm_quote(0.50, 0.1000),
      norm_quote(1.00, 0.0200),
      norm_quote(1.50, 0.0010),
  };
  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::ModelIndependentArbitrage);
  EXPECT_EQ(rep.failure, FeasibilityFailure::SlopeAtZeroBelowMinusOne);
  EXPECT_NEAR(rep.slope_at_zero, (0.10 - 1.0) / 0.50, 1e-12);
  EXPECT_EQ(rep.first_failure_index, 0u);
}

TEST(QuoteFeasibility, SlopeAtZeroExactlyMinusOne_Feasible) {
  // Boundary: r_1 = 1 - x_1 exactly, i.e. the first quote sits ON its forward
  // intrinsic. The bound is inclusive.
  Harness h;
  const std::vector<CallQuote> q{
      norm_quote(0.40, 0.60),
      norm_quote(1.00, 0.05),
  };
  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::Feasible) << to_string(rep.failure);
  EXPECT_NEAR(rep.slope_at_zero, -1.0, 1e-12);
}

TEST(QuoteFeasibility, SlopeAtZeroJustBelowMinusOne_ModelIndependent) {
  // One percent under the same intrinsic. The predicate must separate this from
  // the exactly-on-the-bound case above, not smear them together.
  Harness h;
  const std::vector<CallQuote> q{
      norm_quote(0.40, 0.594),
      norm_quote(1.00, 0.05),
  };
  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::ModelIndependentArbitrage);
  EXPECT_EQ(rep.failure, FeasibilityFailure::SlopeAtZeroBelowMinusOne);
  EXPECT_LT(rep.slope_at_zero, -1.0);
}

TEST(QuoteFeasibility, DeepItmOneTickUnderIntrinsic_TolerancedInPriceNotSlope) {
  // The lowest listed strike is 90% in the money, so x_1 = 0.10 and the mid is
  // pinned to intrinsic. One cent under it must be curable by a one-cent
  // price_tol; if the slack were applied to the SLOPE it would be divided by
  // x_1 and a ten-cent tolerance would be needed to clear a one-cent error.
  Harness h;
  const double x1 = 0.10;
  const double shortfall = 0.01 / (kDf * kF); // one cent, normalised
  const std::vector<CallQuote> q{
      norm_quote(x1, 1.0 - x1 - shortfall),
      norm_quote(1.00, 0.05),
  };

  FeasibilityInputs strict = base_inputs();
  const FeasibilityReport rep_strict = h.run(q, strict);
  EXPECT_EQ(rep_strict.verdict, FeasibilityVerdict::ModelIndependentArbitrage);
  EXPECT_EQ(rep_strict.failure, FeasibilityFailure::SlopeAtZeroBelowMinusOne);
  // The slope is barely under the bound; the price is barely under intrinsic.
  EXPECT_LT(rep_strict.slope_at_zero, -1.0);
  EXPECT_GT(rep_strict.slope_at_zero, -1.01);

  FeasibilityInputs tol = base_inputs();
  tol.price_tol = 2.0 * shortfall; // two cents of quote precision
  const FeasibilityReport rep_tol = h.run(q, tol);
  EXPECT_EQ(rep_tol.verdict, FeasibilityVerdict::Feasible) << to_string(rep_tol.failure);
}

TEST(QuoteFeasibility, NormalisationIsLoadBearing_SameShapeDifferentForward) {
  // Identical currency quotes, two different forwards. Under F = 100 the first
  // strike is 5% OTM and the intrinsic bound is slack; under F = 200 the same
  // strike is 52.5% ITM and the same price is far below intrinsic. A dropped
  // /F or /DF cannot produce both answers.
  Harness h;
  const std::vector<CallQuote> q{
      CallQuote{95.0, 6.00, true, true},
      CallQuote{105.0, 2.00, true, true},
      CallQuote{115.0, 0.50, true, true},
  };

  FeasibilityInputs lo = base_inputs();
  EXPECT_EQ(h.run(q, lo).verdict, FeasibilityVerdict::Feasible);

  FeasibilityInputs hi = base_inputs();
  hi.forward = 200.0;
  const FeasibilityReport rep_hi = h.run(q, hi);
  EXPECT_EQ(rep_hi.verdict, FeasibilityVerdict::ModelIndependentArbitrage);
  EXPECT_EQ(rep_hi.failure, FeasibilityFailure::SlopeAtZeroBelowMinusOne);
}

// ------------------------------------------------------ degenerate: counts --

TEST(QuoteFeasibility, ZeroQuotes_VacuouslyFeasible) {
  std::array<FeasibilityNode, 1> scratch{};
  const FeasibilityReport rep = check_quote_feasibility(std::span<const CallQuote>{}, base_inputs(),
                                                        std::span<FeasibilityNode>{scratch});

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::Feasible);
  EXPECT_EQ(rep.n_points, 0u);
  EXPECT_EQ(rep.slope_at_zero, 0.0);
  EXPECT_EQ(rep.min_drop, 0.0);
  EXPECT_EQ(rep.max_excess, 0.0);
}

TEST(QuoteFeasibility, SingleQuote_ConditionThreeVacuous_Feasible) {
  Harness h;
  const std::vector<CallQuote> q{norm_quote(1.00, 0.08)};
  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::Feasible) << to_string(rep.failure);
  EXPECT_EQ(rep.n_points, 1u);
  EXPECT_EQ(rep.n_weak_points, 0u);
}

TEST(QuoteFeasibility, SingleQuote_BelowIntrinsic_StillModelIndependent) {
  // One quote is enough to be unarbitrageable-by-no-model: conditions 1 and 2
  // bind on the single segment from the adjoined origin.
  Harness h;
  const std::vector<CallQuote> q{norm_quote(0.50, 0.10)};
  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::ModelIndependentArbitrage);
  EXPECT_EQ(rep.failure, FeasibilityFailure::SlopeAtZeroBelowMinusOne);
}

TEST(QuoteFeasibility, SingleQuoteAtOrAboveForwardValue_NotStrictlyDecreasing) {
  // r_1 = 1 means the call is worth the whole discounted forward: the minorant
  // does not decrease at all on [0, x_1].
  Harness h;
  const std::vector<CallQuote> q{norm_quote(1.00, 1.00)};
  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::ModelIndependentArbitrage);
  EXPECT_EQ(rep.failure, FeasibilityFailure::NotStrictlyDecreasing);
}

TEST(QuoteFeasibility, RowsWithNoLiveSide_SkippedNotCounted) {
  Harness h;
  std::vector<CallQuote> q = clean_slice();
  q[1].has_bid = false;
  q[1].has_ask = false;
  q[1].price = 999.0; // would be a gross arbitrage if it were read

  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::Feasible) << to_string(rep.failure);
  EXPECT_EQ(rep.n_points, 4u);
}

TEST(QuoteFeasibility, OneSidedQuote_StillParticipates) {
  Harness h;
  std::vector<CallQuote> q = clean_slice();
  q[1].has_bid = false; // ask-only, but still a quote

  const FeasibilityReport rep = h.run(q, base_inputs());
  EXPECT_EQ(rep.n_points, 5u);
  EXPECT_EQ(rep.verdict, FeasibilityVerdict::Feasible) << to_string(rep.failure);
}

// ------------------------------------------------- degenerate: malformation --

TEST(QuoteFeasibility, DuplicateStrikeEqualPrice_CollapsedToOnePoint) {
  Harness h;
  std::vector<CallQuote> q = clean_slice();
  q.insert(q.begin() + 3, q[2]); // exact duplicate of the x = 1.00 quote

  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::Feasible) << to_string(rep.failure);
  EXPECT_EQ(rep.n_points, 5u); // six rows in, one collapsed
}

TEST(QuoteFeasibility, DuplicateStrikeDifferentPrice_ModelIndependent) {
  Harness h;
  std::vector<CallQuote> q = clean_slice();
  CallQuote dup = q[2];
  dup.price += 1.0;
  q.insert(q.begin() + 3, dup);

  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::ModelIndependentArbitrage);
  EXPECT_EQ(rep.failure, FeasibilityFailure::DuplicateStrikeDisagreement);
  EXPECT_EQ(rep.first_failure_index, 3u);
}

TEST(QuoteFeasibility, UnsortedStrikes_Rejected) {
  Harness h;
  std::vector<CallQuote> q = clean_slice();
  std::swap(q[1], q[2]);

  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::InvalidInput);
  EXPECT_EQ(rep.failure, FeasibilityFailure::UnsortedStrikes);
  EXPECT_EQ(rep.first_failure_index, 2u);
}

TEST(QuoteFeasibility, NonFinitePrice_Rejected) {
  Harness h;
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    std::vector<CallQuote> q = clean_slice();
    q[3].price = bad;
    const FeasibilityReport rep = h.run(q, base_inputs());
    EXPECT_EQ(rep.verdict, FeasibilityVerdict::InvalidInput);
    EXPECT_EQ(rep.failure, FeasibilityFailure::NonFiniteQuote);
    EXPECT_EQ(rep.first_failure_index, 3u);
  }
}

TEST(QuoteFeasibility, NonFiniteStrike_Rejected) {
  Harness h;
  std::vector<CallQuote> q = clean_slice();
  q[4].strike = std::numeric_limits<double>::quiet_NaN();
  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::InvalidInput);
  EXPECT_EQ(rep.failure, FeasibilityFailure::NonFiniteQuote);
  EXPECT_EQ(rep.first_failure_index, 4u);
}

TEST(QuoteFeasibility, NonPositiveStrike_Rejected) {
  Harness h;
  for (const double bad : {0.0, -25.0}) {
    std::vector<CallQuote> q = clean_slice();
    q[0].strike = bad;
    const FeasibilityReport rep = h.run(q, base_inputs());
    EXPECT_EQ(rep.verdict, FeasibilityVerdict::InvalidInput);
    EXPECT_EQ(rep.failure, FeasibilityFailure::NonPositiveStrike);
    EXPECT_EQ(rep.first_failure_index, 0u);
  }
}

TEST(QuoteFeasibility, NegativePrice_ModelIndependent) {
  Harness h;
  std::vector<CallQuote> q = clean_slice();
  q[4].price = -0.01;
  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::ModelIndependentArbitrage);
  EXPECT_EQ(rep.failure, FeasibilityFailure::NegativePrice);
  EXPECT_EQ(rep.first_failure_index, 4u);
}

TEST(QuoteFeasibility, BadForwardOrDiscount_Rejected) {
  Harness h;
  const std::vector<CallQuote> q = clean_slice();
  const double nan = std::numeric_limits<double>::quiet_NaN();

  for (const double f : {0.0, -100.0, nan, std::numeric_limits<double>::infinity()}) {
    FeasibilityInputs in = base_inputs();
    in.forward = f;
    const FeasibilityReport rep = h.run(q, in);
    EXPECT_EQ(rep.verdict, FeasibilityVerdict::InvalidInput);
    EXPECT_EQ(rep.failure, FeasibilityFailure::BadForwardOrDiscount);
  }
  for (const double d : {0.0, -0.99, nan}) {
    FeasibilityInputs in = base_inputs();
    in.discount = d;
    const FeasibilityReport rep = h.run(q, in);
    EXPECT_EQ(rep.verdict, FeasibilityVerdict::InvalidInput);
    EXPECT_EQ(rep.failure, FeasibilityFailure::BadForwardOrDiscount);
  }
}

TEST(QuoteFeasibility, ScratchTooSmall_Rejected) {
  const std::vector<CallQuote> q = clean_slice();
  std::vector<FeasibilityNode> scratch(q.size()); // one short of n + 1
  const FeasibilityReport rep = check_quote_feasibility(
      std::span<const CallQuote>{q}, base_inputs(), std::span<FeasibilityNode>{scratch});

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::InvalidInput);
  EXPECT_EQ(rep.failure, FeasibilityFailure::ScratchTooSmall);
}

TEST(QuoteFeasibility, ScratchSizeRule_ExactlyNPlusOneSuffices) {
  const std::vector<CallQuote> q = clean_slice();
  EXPECT_EQ(feasibility_scratch_size(q.size()), q.size() + 1u);
  std::vector<FeasibilityNode> scratch(feasibility_scratch_size(q.size()));
  const FeasibilityReport rep = check_quote_feasibility(
      std::span<const CallQuote>{q}, base_inputs(), std::span<FeasibilityNode>{scratch});
  EXPECT_EQ(rep.verdict, FeasibilityVerdict::Feasible) << to_string(rep.failure);
}

TEST(QuoteFeasibility, EmptyQuotesWithEmptyScratchAndBadInputs_StillFeasible) {
  // An empty slice is answered before any input validation: no scratch, no
  // forward and no discount are required to say "nothing to contradict".
  FeasibilityInputs junk;
  junk.forward = 0.0;
  junk.discount = 0.0;
  const FeasibilityReport rep =
      check_quote_feasibility(std::span<const CallQuote>{}, junk, std::span<FeasibilityNode>{});
  EXPECT_EQ(rep.verdict, FeasibilityVerdict::Feasible);
  EXPECT_EQ(rep.n_points, 0u);
}

// ----------------------------------------------------------------- strings --

TEST(QuoteFeasibility, VerdictAndFailureStringsAreDistinct) {
  const std::array<FeasibilityVerdict, 4> verdicts{
      FeasibilityVerdict::Feasible, FeasibilityVerdict::WeakArbitrage,
      FeasibilityVerdict::ModelIndependentArbitrage, FeasibilityVerdict::InvalidInput};
  for (std::size_t i = 0; i < verdicts.size(); ++i) {
    EXPECT_NE(to_string(verdicts[i]), nullptr);
    for (std::size_t j = i + 1; j < verdicts.size(); ++j) {
      EXPECT_STRNE(to_string(verdicts[i]), to_string(verdicts[j]));
    }
  }
  EXPECT_STREQ(to_string(FeasibilityFailure::None), "None");
  EXPECT_STRNE(to_string(FeasibilityFailure::NotStrictlyDecreasing),
               to_string(FeasibilityFailure::AboveConvexMinorant));
}

// ------------------------------------------------------------- scale/stress --

// r(x) = 1/(1+x) is strictly decreasing, strictly convex, and has
// r'(0+) slack against the -1 bound over this window.
[[nodiscard]] std::vector<CallQuote> dense_board() {
  std::vector<CallQuote> q;
  q.reserve(400);
  for (int i = 0; i < 400; ++i) {
    const double x = 0.20 + 0.005 * static_cast<double>(i);
    q.push_back(norm_quote(x, 1.0 / (1.0 + x)));
  }
  return q;
}

TEST(QuoteFeasibility, DenseBoard_HullKeepsEveryPoint_Feasible) {
  // 400 strikes on a strictly convex curve; the monotone chain must not pop a
  // point it needs, and the scratch bound must hold at the cap.
  Harness h;
  const FeasibilityReport rep = h.run(dense_board(), base_inputs());

  EXPECT_EQ(rep.n_points, 400u);
  EXPECT_EQ(rep.verdict, FeasibilityVerdict::Feasible) << to_string(rep.failure);
  EXPECT_EQ(rep.n_weak_points, 0u);
}

TEST(QuoteFeasibility, DenseBoard_SingleInteriorBumpIsolated) {
  Harness h;
  std::vector<CallQuote> q = dense_board();
  q[137].price *= 1.02; // lift one strike off the convex curve

  const FeasibilityReport rep = h.run(q, base_inputs());

  EXPECT_EQ(rep.verdict, FeasibilityVerdict::WeakArbitrage);
  EXPECT_EQ(rep.n_weak_points, 1u);
  EXPECT_EQ(rep.first_failure_index, 137u);
}

} // namespace
