// cboe_strip.hpp gate tests -- the published discrete-strike variance sum.
//
// PROVENANCE OF THE WORKED EXAMPLE -- READ THIS BEFORE TRUSTING THE NUMBERS.
//
// The task called for reproducing the 2019 CBOE VIX white paper's own worked
// example from hand-transcribed constants. That example's inputs are a board of
// roughly a hundred listed strikes with per-strike bid/ask on both legs, plus
// the published per-strike dK and contribution columns. Those constants are NOT
// transcribed here, because they could not be recovered with the confidence a
// citation demands: quoting a "published" figure that is actually a
// reconstruction would make this test look like an external check while being
// nothing of the kind, and a test that lies about its own authority is worse
// than no test.
//
// `WorkedExample_HandDerived` below is therefore exactly what its name says: a
// SIX-STRIKE board small enough to carry the whole derivation in the test, with
// every dK, every Q(K), and every summand written out as an arithmetic literal
// and summed independently of the implementation. It pins the formula, the
// endpoint dK rule, the out-of-the-money selection, the K_0 average and the
// Taylor term against numbers a reader can check with a calculator -- it does
// NOT pin this library against the exchange. Cross-checking against the real
// published example remains open, and wants the actual PDF in hand.
//
// The board is arbitrage-coherent on purpose: every strike satisfies
// C_mid - P_mid == df * (F - K) exactly at F = 104, df = 0.95, so nothing in
// the fixture is quietly impossible.
//
// Coverage:
//   1. WorkedExample_HandDerived      -- the full sum, term by term.
//   2. OtmSelection_*                 -- puts below K0, calls above, average at
//                                        K0, and the wrong-side legs proven
//                                        unread.
//   3. ZeroBidTruncation_*            -- the two-consecutive-zero rule changing
//                                        the answer, and its flag.
//   4. K0Selection_*                  -- at, just below and just above F.
//   5. EndpointDeltaK                 -- the one-sided rule at both strip ends.
//   6. DiscountFactor_*               -- 1/df actually scales the sum.
//   7. Degenerate_*                   -- empty / single / all-bidless / malformed
//                                        boards return a Status, never a NaN.
//   8. Basis_*                        -- the parametric-vs-listed diagnostic.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/cboe_strip.hpp"
#include "atx/vol/types.hpp" // ErrorCode

using atx::vol::CboeStrikeQuote;
using atx::vol::CboeStripLeg;
using atx::vol::cboe_parametric_basis;
using atx::vol::cboe_var_strike;
using atx::vol::ErrorCode;

namespace {

// Every field is a REAL parameter. No neutral default hides behind this helper:
// a caller must state both sides of both legs, so no test built on it can be
// blind to the mid convention, to the bid the truncation rule reads, or to the
// wrong-side legs the selection rule must ignore.
[[nodiscard]] CboeStrikeQuote row(double strike, double call_bid, double call_ask, double put_bid,
                                  double put_ask) {
  CboeStrikeQuote q{};
  q.strike = strike;
  q.call_bid = call_bid;
  q.call_ask = call_ask;
  q.put_bid = put_bid;
  q.put_ask = put_ask;
  return q;
}

constexpr double kF = 104.0;   // forward
constexpr double kDf = 0.95;   // discount factor; the published e^{RT} is 1/df
constexpr double kT = 0.25;    // years

// The worked-example board. Strikes are DELIBERATELY unevenly spaced (15, 10,
// 5, 10, 20) so the midpoint rule and the one-sided endpoint rule produce
// different numbers at every position -- on an evenly spaced board the two
// rules agree at the ends and the test would be blind to the endpoint
// convention entirely.
[[nodiscard]] std::vector<CboeStrikeQuote> worked_board() {
  return {
      //   K     call_bid call_ask  put_bid put_ask
      row(70.0, 32.30, 32.70, 0.10, 0.30),
      row(85.0, 18.55, 18.95, 0.50, 0.90),
      row(95.0, 10.10, 10.60, 1.60, 2.00),
      row(100.0, 6.20, 6.60, 2.40, 2.80),
      row(110.0, 1.90, 2.30, 7.60, 8.00),
      row(130.0, 0.10, 0.30, 24.70, 25.10),
  };
}

} // namespace

// ── 1. The hand-derived worked example ──────────────────────────────────────

TEST(CboeStrip, WorkedExample_HandDerived) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();

  // F = 104 sits between the 100 and 110 strikes, so K0 = 100.
  EXPECT_DOUBLE_EQ(got->k0, 100.0);
  EXPECT_EQ(got->n_puts, 3u);  // 70, 85, 95
  EXPECT_EQ(got->n_calls, 2u); // 110, 130
  ASSERT_EQ(got->terms.size(), 6u);

  // Every published intermediate, by hand.
  //
  //   K     leg      Q(K)                       dK
  //   70    put      (0.10 + 0.30)/2 = 0.20     85 - 70          = 15      (endpoint)
  //   85    put      (0.50 + 0.90)/2 = 0.70     (95 - 70)/2      = 12.5
  //   95    put      (1.60 + 2.00)/2 = 1.80     (100 - 85)/2     = 7.5
  //  100    avg      ((2.40+2.80)/2 + (6.20+6.60)/2)/2
  //                  = (2.60 + 6.40)/2 = 4.50   (110 - 95)/2     = 7.5
  //  110    call     (1.90 + 2.30)/2 = 2.10     (130 - 100)/2    = 15
  //  130    call     (0.10 + 0.30)/2 = 0.20     130 - 110        = 20      (endpoint)
  const double k[6] = {70.0, 85.0, 95.0, 100.0, 110.0, 130.0};
  const double dk[6] = {15.0, 12.5, 7.5, 7.5, 15.0, 20.0};
  const double q[6] = {0.20, 0.70, 1.80, 4.50, 2.10, 0.20};
  const CboeStripLeg leg[6] = {CboeStripLeg::Put,  CboeStripLeg::Put,
                               CboeStripLeg::Put,  CboeStripLeg::K0Average,
                               CboeStripLeg::Call, CboeStripLeg::Call};
  for (std::size_t i = 0; i < 6u; ++i) {
    EXPECT_DOUBLE_EQ(got->terms[i].strike, k[i]) << "strike at term " << i;
    EXPECT_DOUBLE_EQ(got->terms[i].delta_k, dk[i]) << "dK at term " << i;
    EXPECT_DOUBLE_EQ(got->terms[i].mid, q[i]) << "Q(K) at term " << i;
    EXPECT_EQ(got->terms[i].leg, leg[i]) << "leg at term " << i;
  }

  // sigma^2 = (2/T) * SUM (dK/K^2) * (1/df) * Q  -  (1/T) * (F/K0 - 1)^2,
  // summed here from the literals above and NOT from anything the library
  // computed.
  const double raw_sum = (15.0 * 0.20) / (70.0 * 70.0)      //  6.122448979591837e-4
                         + (12.5 * 0.70) / (85.0 * 85.0)    //  1.211072664359862e-3
                         + (7.5 * 1.80) / (95.0 * 95.0)     //  1.495844875346260e-3
                         + (7.5 * 4.50) / (100.0 * 100.0)   //  3.375000000000000e-3
                         + (15.0 * 2.10) / (110.0 * 110.0)  //  2.603305785123967e-3
                         + (20.0 * 0.20) / (130.0 * 130.0); //  2.366863905325444e-4
  const double expected_sum_term = (2.0 / 0.25) * (raw_sum / 0.95);
  // (104/100 - 1)^2 = 0.04^2 = 0.0016; times 1/T = 4.
  const double expected_taylor = -(1.0 / 0.25) * 0.0016;
  const double expected_var = expected_sum_term + expected_taylor;

  EXPECT_NEAR(got->sum_term, expected_sum_term, 1e-13);
  // NEAR and not DOUBLE_EQ, for a reason worth stating: the hand derivation
  // writes (104/100 - 1)^2 as 0.04^2 because that is the arithmetic, but 1.04
  // has no exact binary representation, so the library's own 104.0/100.0 - 1.0
  // is 0.040000000000000036. The two differ by ~1.1e-17 -- thirteen ULP at this
  // magnitude, hence past DOUBLE_EQ's four -- and that gap is representation,
  // not formula. `K0Selection_ForwardExactlyOnAStrikeZeroesTheTaylorTerm` is
  // where the term is pinned EXACTLY, because there the arithmetic is exact.
  EXPECT_NEAR(got->taylor_term, expected_taylor, 1e-15);
  EXPECT_NEAR(got->var_strike_dec, expected_var, 1e-13);
  EXPECT_DOUBLE_EQ(got->vol_strike_dec, std::sqrt(got->var_strike_dec));

  // The reported decomposition must actually decompose: var == sum + taylor to
  // the last bit, and sum_term == (2/T) * SUM(contribution) to the last bit --
  // the audit trail is only worth publishing if it reconstructs the answer.
  EXPECT_DOUBLE_EQ(got->var_strike_dec, got->sum_term + got->taylor_term);
  double contrib_sum = 0.0;
  for (const auto &term : got->terms) {
    contrib_sum += term.contribution;
  }
  EXPECT_DOUBLE_EQ(got->sum_term, (2.0 / kT) * contrib_sum);

  // Neither wing hit the zero-bid rule; both simply ran off the board.
  EXPECT_FALSE(got->zero_bid_truncated_low);
  EXPECT_FALSE(got->zero_bid_truncated_high);
  EXPECT_DOUBLE_EQ(got->k_lo, 70.0);
  EXPECT_DOUBLE_EQ(got->k_hi, 130.0);
}

// ── 2. Out-of-the-money selection ───────────────────────────────────────────

// The in-the-money legs must never be read. Corrupting them to values that
// would blow the answer apart if used leaves the strike bit-identical.
TEST(CboeStrip, OtmSelection_IgnoresTheInTheMoneyLegs) {
  const std::vector<CboeStrikeQuote> clean = worked_board();
  const auto baseline = cboe_var_strike(clean, kF, kDf, kT);
  ASSERT_TRUE(baseline.has_value()) << baseline.error().to_string();

  std::vector<CboeStrikeQuote> poisoned = clean;
  poisoned[0].call_bid = 900.0; // calls below K0 are in-the-money: unread
  poisoned[0].call_ask = 901.0;
  poisoned[1].call_bid = 800.0;
  poisoned[1].call_ask = 801.0;
  poisoned[2].call_bid = 700.0;
  poisoned[2].call_ask = 701.0;
  poisoned[4].put_bid = 600.0; // puts above K0 are in-the-money: unread
  poisoned[4].put_ask = 601.0;
  poisoned[5].put_bid = 500.0;
  poisoned[5].put_ask = 501.0;

  const auto got = cboe_var_strike(poisoned, kF, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_DOUBLE_EQ(got->var_strike_dec, baseline->var_strike_dec);
}

// K_0 is the one strike that contributes BOTH legs, averaged. Moving the K_0
// CALL alone must move the answer -- if the implementation took only the put
// there (or only the call), this is the test that fails.
TEST(CboeStrip, OtmSelection_K0AveragesBothLegs) {
  const std::vector<CboeStrikeQuote> clean = worked_board();
  const auto baseline = cboe_var_strike(clean, kF, kDf, kT);
  ASSERT_TRUE(baseline.has_value()) << baseline.error().to_string();
  EXPECT_DOUBLE_EQ(baseline->terms[3].mid, 4.50); // (2.60 + 6.40)/2

  std::vector<CboeStrikeQuote> bumped = clean;
  bumped[3].call_bid += 1.00; // K0 call mid 6.40 -> 7.40, so Q(K0) 4.50 -> 5.00
  bumped[3].call_ask += 1.00;
  const auto got = cboe_var_strike(bumped, kF, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_DOUBLE_EQ(got->terms[3].mid, 5.00);

  // Only the K0 term moved: d(sigma^2) = (2/T)(dK0/K0^2)(1/df) * dQ.
  const double expected_delta = (2.0 / kT) * (7.5 / (100.0 * 100.0)) * (1.0 / kDf) * 0.50;
  EXPECT_NEAR(got->var_strike_dec - baseline->var_strike_dec, expected_delta, 1e-13);
}

// Q(K) is the MIDPOINT, not the bid. Widening one ask alone must move the mark.
TEST(CboeStrip, OtmSelection_UsesMidNotBid) {
  const std::vector<CboeStrikeQuote> clean = worked_board();
  const auto baseline = cboe_var_strike(clean, kF, kDf, kT);
  ASSERT_TRUE(baseline.has_value()) << baseline.error().to_string();

  std::vector<CboeStrikeQuote> wide = clean;
  wide[2].put_ask += 0.40; // 95 put mid 1.80 -> 2.00
  const auto got = cboe_var_strike(wide, kF, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_DOUBLE_EQ(got->terms[2].mid, 2.00);

  const double expected_delta = (2.0 / kT) * (7.5 / (95.0 * 95.0)) * (1.0 / kDf) * 0.20;
  EXPECT_NEAR(got->var_strike_dec - baseline->var_strike_dec, expected_delta, 1e-13);
}

// ── 3. Zero-bid truncation ──────────────────────────────────────────────────

namespace {

// Two boards identical except for the 65-strike PUT BID. In `truncating`, the
// 80 and 65 puts are both bidless and consecutive, so the rule stops the walk
// and the perfectly well-quoted 60 put is DISCARDED. In `surviving`, the 65 put
// has a bid, the run resets, and both 65 and 60 are admitted. The 60 put is
// quoted in BOTH boards -- that is the point: the only thing separating the two
// answers is the rule itself.
[[nodiscard]] std::vector<CboeStrikeQuote> truncation_board(double k65_put_bid) {
  return {
      //   K      call_bid call_ask  put_bid  put_ask
      row(60.0, 41.90, 42.50, 0.05, 0.25),
      row(65.0, 37.20, 37.80, k65_put_bid, 0.30),
      row(80.0, 23.00, 23.60, 0.00, 0.30), // bidless
      row(90.0, 13.90, 14.50, 0.60, 1.00),
      row(95.0, 9.60, 10.10, 1.60, 2.00),
      row(100.0, 6.20, 6.60, 2.40, 2.80),
      row(110.0, 1.90, 2.30, 7.60, 8.00),
  };
}

} // namespace

TEST(CboeStrip, ZeroBidTruncation_StopsTheWalkAndChangesTheAnswer) {
  const std::vector<CboeStrikeQuote> truncating = truncation_board(0.00);
  const std::vector<CboeStrikeQuote> surviving = truncation_board(0.10);

  const auto cut = cboe_var_strike(truncating, kF, kDf, kT);
  ASSERT_TRUE(cut.has_value()) << cut.error().to_string();
  const auto full = cboe_var_strike(surviving, kF, kDf, kT);
  ASSERT_TRUE(full.has_value()) << full.error().to_string();

  // Truncated: the walk stopped at the 80/65 consecutive no-bid pair, so 60
  // never entered even though it is quoted at 0.05/0.25.
  EXPECT_TRUE(cut->zero_bid_truncated_low);
  EXPECT_FALSE(cut->zero_bid_truncated_high);
  EXPECT_EQ(cut->n_puts, 2u);
  ASSERT_EQ(cut->terms.size(), 4u);
  const double cut_k[4] = {90.0, 95.0, 100.0, 110.0};
  const double cut_dk[4] = {5.0, 5.0, 7.5, 10.0}; // 95-90, (100-90)/2, (110-95)/2, 110-100
  for (std::size_t i = 0; i < 4u; ++i) {
    EXPECT_DOUBLE_EQ(cut->terms[i].strike, cut_k[i]) << "strike at term " << i;
    EXPECT_DOUBLE_EQ(cut->terms[i].delta_k, cut_dk[i]) << "dK at term " << i;
  }

  // Not truncated: the 65 bid resets the run, so 65 AND 60 are admitted, and
  // the surviving-strip dK at 90 widens from 5 to 15 because its lower
  // neighbour is now 65 rather than 95's own gap.
  EXPECT_FALSE(full->zero_bid_truncated_low);
  EXPECT_EQ(full->n_puts, 4u);
  ASSERT_EQ(full->terms.size(), 6u);
  const double full_k[6] = {60.0, 65.0, 90.0, 95.0, 100.0, 110.0};
  const double full_dk[6] = {5.0, 15.0, 15.0, 5.0, 7.5, 10.0};
  for (std::size_t i = 0; i < 6u; ++i) {
    EXPECT_DOUBLE_EQ(full->terms[i].strike, full_k[i]) << "strike at term " << i;
    EXPECT_DOUBLE_EQ(full->terms[i].delta_k, full_dk[i]) << "dK at term " << i;
  }

  // The rule is load-bearing, not cosmetic: discarding the far wing lowers the
  // strike materially.
  EXPECT_LT(cut->var_strike_dec, full->var_strike_dec);
  EXPECT_GT(full->var_strike_dec - cut->var_strike_dec, 1e-4);
}

// A LONE bidless strike is skipped but does not stop the walk -- only a
// consecutive pair does. The 80 put is bidless in both boards above and neither
// walk stopped there.
TEST(CboeStrip, ZeroBidTruncation_SingleGapDoesNotStopTheWalk) {
  const std::vector<CboeStrikeQuote> board = truncation_board(0.10);
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  for (const auto &term : got->terms) {
    EXPECT_NE(term.strike, 80.0) << "the bidless 80 put must be excluded";
  }
  EXPECT_EQ(got->n_puts, 4u); // 60, 65, 90, 95 -- the walk crossed the gap
}

// The call wing obeys the same rule, keyed on the CALL bid.
TEST(CboeStrip, ZeroBidTruncation_AppliesToTheCallWing) {
  std::vector<CboeStrikeQuote> board = worked_board();
  board.push_back(row(150.0, 0.05, 0.25, 45.00, 45.60)); // quoted, but unreachable
  board[4].call_bid = 0.0;                               // 110 call bidless
  board[5].call_bid = 0.0;                               // 130 call bidless -> stop

  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_TRUE(got->zero_bid_truncated_high);
  EXPECT_EQ(got->n_calls, 0u);
  EXPECT_DOUBLE_EQ(got->k_hi, 100.0); // the strip ends at K0
}

// ── 4. K0 selection at and near F ───────────────────────────────────────────

TEST(CboeStrip, K0Selection_ForwardExactlyOnAStrikeZeroesTheTaylorTerm) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const auto got = cboe_var_strike(board, 100.0, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_DOUBLE_EQ(got->k0, 100.0);
  // Exactly zero, not merely small: the term corrects a gap that is not there.
  EXPECT_EQ(got->taylor_term, 0.0);
  EXPECT_DOUBLE_EQ(got->var_strike_dec, got->sum_term);
}

TEST(CboeStrip, K0Selection_JustBelowAStrikeStepsDown) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const auto got = cboe_var_strike(board, 99.999, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_DOUBLE_EQ(got->k0, 95.0);
  EXPECT_EQ(got->n_puts, 2u);  // 70, 85
  EXPECT_EQ(got->n_calls, 3u); // 100, 110, 130
  EXPECT_EQ(got->terms[2].leg, CboeStripLeg::K0Average);
  EXPECT_EQ(got->terms[3].leg, CboeStripLeg::Call); // 100 is now a CALL
}

TEST(CboeStrip, K0Selection_JustAboveAStrikeStaysPut) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const auto got = cboe_var_strike(board, 100.001, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_DOUBLE_EQ(got->k0, 100.0);
  EXPECT_GT(got->taylor_term, -1e-6);
  EXPECT_LT(got->taylor_term, 0.0);
}

TEST(CboeStrip, K0Selection_ForwardAboveTheBoardLeavesNoCallWing) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const auto got = cboe_var_strike(board, 132.0, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_DOUBLE_EQ(got->k0, 130.0);
  EXPECT_EQ(got->n_calls, 0u);
  EXPECT_EQ(got->n_puts, 5u);
  EXPECT_FALSE(got->zero_bid_truncated_high); // exhausted, not truncated
}

TEST(CboeStrip, K0Selection_ForwardBelowTheBoardIsOutOfRange) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const auto got = cboe_var_strike(board, 69.9, kDf, kT);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::OutOfRange);
}

// ── 5. Endpoint dK ──────────────────────────────────────────────────────────

// Three strikes, wildly asymmetric spacing, F on the middle one. Under the
// one-sided endpoint rule the ends get 10 and 30; under a (wrong) midpoint rule
// applied at the ends they would be 5 and 15.
TEST(CboeStrip, EndpointDeltaK) {
  const std::vector<CboeStrikeQuote> board = {
      row(90.0, 12.10, 12.60, 1.20, 1.60),
      row(100.0, 5.30, 5.70, 5.30, 5.70),
      row(130.0, 0.30, 0.50, 29.00, 29.60),
  };
  const auto got = cboe_var_strike(board, 100.0, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  ASSERT_EQ(got->terms.size(), 3u);
  EXPECT_DOUBLE_EQ(got->terms[0].delta_k, 10.0); // 100 - 90        (lowest)
  EXPECT_DOUBLE_EQ(got->terms[1].delta_k, 20.0); // (130 - 90)/2    (interior)
  EXPECT_DOUBLE_EQ(got->terms[2].delta_k, 30.0); // 130 - 100       (highest)
}

// ── 6. The discount factor ──────────────────────────────────────────────────

// 1/df multiplies the sum and nothing else, so sum_term(df) * df is invariant
// and the Taylor term does not move at all. A test suite whose fixture pinned
// df to 1.0 would never notice the factor being dropped.
TEST(CboeStrip, DiscountFactor_ScalesTheSumAndNotTheTaylorTerm) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const auto discounted = cboe_var_strike(board, kF, 0.95, kT);
  ASSERT_TRUE(discounted.has_value()) << discounted.error().to_string();
  const auto undiscounted = cboe_var_strike(board, kF, 1.0, kT);
  ASSERT_TRUE(undiscounted.has_value()) << undiscounted.error().to_string();

  EXPECT_NEAR(discounted->sum_term * 0.95, undiscounted->sum_term, 1e-14);
  EXPECT_DOUBLE_EQ(discounted->taylor_term, undiscounted->taylor_term);
  EXPECT_GT(discounted->var_strike_dec, undiscounted->var_strike_dec);
}

// ── 7. Degenerate and malformed boards ──────────────────────────────────────

TEST(CboeStrip, Degenerate_EmptyBoard) {
  const auto got = cboe_var_strike(std::span<const CboeStrikeQuote>{}, kF, kDf, kT);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::InvalidArgument);
}

TEST(CboeStrip, Degenerate_SingleStrike) {
  const std::vector<CboeStrikeQuote> board = {row(100.0, 6.20, 6.60, 2.40, 2.80)};
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::InvalidArgument);
}

// Both wings die on the first consecutive pair, leaving only K0 -- one strike,
// and one strike has no dK. This must be a Status, not a NaN strike.
TEST(CboeStrip, Degenerate_AllZeroBid) {
  const std::vector<CboeStrikeQuote> board = {
      row(80.0, 0.00, 24.00, 0.00, 0.40), row(90.0, 0.00, 14.50, 0.00, 1.00),
      row(100.0, 0.00, 6.60, 0.00, 2.80), row(110.0, 0.00, 2.30, 0.00, 8.00),
      row(120.0, 0.00, 0.80, 0.00, 16.50),
  };
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::InvalidArgument);
}

TEST(CboeStrip, Degenerate_NonAscendingStrikes) {
  std::vector<CboeStrikeQuote> board = worked_board();
  std::swap(board[1], board[2]);
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::InvalidArgument);
}

TEST(CboeStrip, Degenerate_DuplicateStrikes) {
  std::vector<CboeStrikeQuote> board = worked_board();
  board[2].strike = board[1].strike;
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::InvalidArgument);
}

TEST(CboeStrip, Degenerate_CrossedQuote) {
  std::vector<CboeStrikeQuote> board = worked_board();
  board[2].put_ask = board[2].put_bid - 0.10;
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::InvalidArgument);
}

TEST(CboeStrip, Degenerate_NonFiniteQuote) {
  std::vector<CboeStrikeQuote> board = worked_board();
  board[4].call_ask = std::numeric_limits<double>::quiet_NaN();
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::InvalidArgument);
}

TEST(CboeStrip, Degenerate_NonPositiveStrike) {
  std::vector<CboeStrikeQuote> board = worked_board();
  board[0].strike = 0.0;
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::InvalidArgument);
}

TEST(CboeStrip, Degenerate_BadScalarArguments) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const double bad_f : {0.0, -104.0, nan}) {
    const auto got = cboe_var_strike(board, bad_f, kDf, kT);
    ASSERT_FALSE(got.has_value()) << "forward " << bad_f;
    EXPECT_EQ(got.error().code(), ErrorCode::InvalidArgument);
  }
  for (const double bad_df : {0.0, -0.95, nan}) {
    const auto got = cboe_var_strike(board, kF, bad_df, kT);
    ASSERT_FALSE(got.has_value()) << "df " << bad_df;
    EXPECT_EQ(got.error().code(), ErrorCode::InvalidArgument);
  }
  for (const double bad_t : {0.0, -0.25, nan}) {
    const auto got = cboe_var_strike(board, kF, kDf, bad_t);
    ASSERT_FALSE(got.has_value()) << "T " << bad_t;
    EXPECT_EQ(got.error().code(), ErrorCode::InvalidArgument);
  }
}

// A forward far above a thin board makes the Taylor term outrun the whole sum.
// That is not a mark -- it is an OutOfRange, so no caller ever receives a NaN
// vol from a sqrt of a negative variance.
TEST(CboeStrip, Degenerate_NegativeVarianceIsRejected) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const auto got = cboe_var_strike(board, 200.0, kDf, kT);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::OutOfRange);
}

// ── 8. The basis diagnostic ─────────────────────────────────────────────────

TEST(CboeStrip, Basis_ReportsBothVarianceAndVolGaps) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const auto strip = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_TRUE(strip.has_value()) << strip.error().to_string();

  constexpr double kParametric = 0.0625; // a 25-vol parametric strike
  const auto got = cboe_parametric_basis(board, kF, kDf, kT, kParametric);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();

  EXPECT_DOUBLE_EQ(got->cboe_var_dec, strip->var_strike_dec);
  EXPECT_DOUBLE_EQ(got->parametric_var_dec, kParametric);
  EXPECT_DOUBLE_EQ(got->basis_var_dec, strip->var_strike_dec - kParametric);
  EXPECT_DOUBLE_EQ(got->basis_vol_dec, strip->vol_strike_dec - 0.25);
  // The audit trail travels with the diagnostic.
  ASSERT_EQ(got->strip.terms.size(), strip->terms.size());
  EXPECT_DOUBLE_EQ(got->strip.k0, strip->k0);
}

TEST(CboeStrip, Basis_RejectsAnUnusableParametricStrike) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const double bad : {-1.0e-9, nan}) {
    const auto got = cboe_parametric_basis(board, kF, kDf, kT, bad);
    ASSERT_FALSE(got.has_value()) << "parametric " << bad;
    EXPECT_EQ(got.error().code(), ErrorCode::InvalidArgument);
  }
}

TEST(CboeStrip, Basis_PropagatesABadBoard) {
  const auto got = cboe_parametric_basis(std::span<const CboeStrikeQuote>{}, kF, kDf, kT, 0.04);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::InvalidArgument);
}
