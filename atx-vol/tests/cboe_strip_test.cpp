// cboe_strip.hpp gate tests -- the published discrete-strike variance sum.
//
// TWO ORACLES, WITH DIFFERENT AUTHORITY. Read this before trusting either.
//
// 1. `WhitePaper_*` is a CITED reproduction of Cboe's own published worked
//    example, transcribed from:
//
//      Cboe Global Indices, "Cboe Volatility Index -- Methodology",
//        Version 6.0, revised 2026-02-26, Appendices 3-5.  [CUR-V]
//      Cboe Global Indices, "Cboe Volatility Index -- Mathematics Methodology",
//        Version 5.0, revised 2026-02-26, §3(a), §5(b).    [CUR-M]
//      https://cdn.cboe.com/api/global/us_indices/governance/
//        Volatility_Index_Methodology_Cboe_Volatility_Index.pdf
//      https://cdn.cboe.com/api/global/us_indices/governance/
//        Cboe_Volatility_Index_Mathematics_Methodology.pdf
//
//    Hypothetical SPX calculation for 2022-09-27 10:45:15 ET. All 268 published
//    strip rows (146 near-term, 122 next-term) with their published Q(K) and dK
//    are below, and both published sigma^2 values are asserted. This is what
//    pins this library against the exchange.
//
// 2. `WorkedExample_HandDerived` is a SIX-STRIKE board with the whole derivation
//    written out as arithmetic literals. It pins the formula against numbers a
//    reader can check on a calculator, independently of any transcription. It is
//    kept because it fails for a DIFFERENT reason than the cited test would: a
//    transcription error breaks the cited test and leaves this one green, and a
//    formula error breaks both.
//
// WHAT THE CITED TEST DOES NOT COVER, stated so nobody assumes otherwise:
//   - [CUR-V] Appendix 4's raw bid/ask board runs to several hundred rows and is
//     NOT transcribed. Appendix 5 publishes the MIDPOINT Q(K) per strike, so
//     strip members are fed as bid == ask == Q, EXCEPT the strikes whose raw
//     quotes Appendix 3 does publish (`kNearRawInStrip`, the exclusion-boundary
//     rows), which are fed as published bid/ask and whose midpoints are then
//     asserted to reproduce the published Q. The mid convention itself is pinned
//     discriminatingly by `OtmSelection_UsesMidNotBid`, not here.
//   - A few excluded board rows are published with a zero BID but no ask; see
//     `kUnpublishedAsk`.
//   - The 2025-02-10 zero-ASK exclusion has published prose but NO published
//     worked example anywhere, so it is pinned by construction tests only
//     (`ZeroQuote_*`) and is never claimed to be verified against Cboe numbers.
//
// TOLERANCES. sigma^2 is asserted at 1e-8 and MUST NOT be tightened. That floor
// is the SOURCE's precision, not this library's: Cboe prints the `(2/T)*Sum`
// line rounded to 6 decimal places while carrying full precision internally, so
// the published sigma^2 lines are not internally consistent at ~1e-8 (0.019267 -
// 0.00003337 = 0.01923363, but the published sigma^2 is 0.019233906). The strip
// sum, which Appendix 5 prints to 10 dp, IS asserted tightly at 1e-10.
//
// Other coverage:
//   OtmSelection_*  puts below K0, calls above, the K0 average, wrong-side legs
//                   proven unread, mid-not-bid.
//   ZeroQuote_*     the two-consecutive rule truncating; a lone gap not
//                   truncating; the call wing; the 2025 zero-ASK half.
//   K0Selection_*   at, just below and just above F.
//   EndpointDeltaK  the one-sided rule at both strip ends.
//   Scaling_*       1/df and 1/T each proven to move the answer.
//   NotCalculable_* [CUR-M] §5(b): empty wing, null K0 leg.
//   Degenerate_*    malformed boards return a Status, never a NaN.
//   Basis_*         the parametric-vs-listed diagnostic.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/cboe_strip.hpp"
#include "atx/vol/types.hpp" // ErrorCode

using atx::vol::CboeStrikeQuote;
using atx::vol::CboeStripLeg;
using atx::vol::CboeVarStrip;
using atx::vol::cboe_parametric_basis;
using atx::vol::cboe_var_strike;
using atx::vol::ErrorCode;

namespace {

// Every field is a REAL parameter. No neutral default hides behind this helper:
// a caller must state both sides of both legs, so no test built on it can be
// blind to the mid convention, to the quotes the exclusion rule reads, or to the
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

constexpr double kF = 104.0; // forward
constexpr double kDf = 0.95; // discount factor; the published e^{RT} is 1/df
constexpr double kT = 0.25;  // years

// The hand-derived board. Strikes are DELIBERATELY unevenly spaced (15, 10, 5,
// 10, 20) so the midpoint rule and the one-sided endpoint rule produce different
// numbers at every position -- on an evenly spaced board the two rules agree at
// the ends and the test would be blind to the endpoint convention entirely.
// Arbitrage-coherent: C_mid - P_mid == df*(F - K) exactly at all six strikes.
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

  // Neither wing hit the exclusion rule; both simply ran off the board.
  EXPECT_FALSE(got->zero_quote_truncated_low);
  EXPECT_FALSE(got->zero_quote_truncated_high);
  EXPECT_DOUBLE_EQ(got->k_lo, 70.0);
  EXPECT_DOUBLE_EQ(got->k_hi, 130.0);
}

// ── 2. Out-of-the-money selection ───────────────────────────────────────────

// The in-the-money legs must never be read. Corrupting them to values that would
// blow the answer apart if used leaves the strike bit-identical.
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

// ── 3. Zero-quote exclusion and truncation ──────────────────────────────────

namespace {

// Two boards identical except for the 65-strike PUT BID. In `truncating`, the 80
// and 65 puts are both bidless and consecutive, so the rule stops the walk and
// the perfectly well-quoted 60 put is DISCARDED. In `surviving`, the 65 put has
// a bid, the run resets, and both 65 and 60 are admitted. The 60 put is quoted in
// BOTH boards -- that is the point: the only thing separating the two answers is
// the rule itself.
[[nodiscard]] std::vector<CboeStrikeQuote> truncation_board(double k65_put_bid) {
  return {
      //   K      call_bid call_ask  put_bid      put_ask
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

TEST(CboeStrip, ZeroQuote_TruncationStopsTheWalkAndChangesTheAnswer) {
  const std::vector<CboeStrikeQuote> truncating = truncation_board(0.00);
  const std::vector<CboeStrikeQuote> surviving = truncation_board(0.10);

  const auto cut = cboe_var_strike(truncating, kF, kDf, kT);
  ASSERT_TRUE(cut.has_value()) << cut.error().to_string();
  const auto full = cboe_var_strike(surviving, kF, kDf, kT);
  ASSERT_TRUE(full.has_value()) << full.error().to_string();

  // Truncated: the walk stopped at the 80/65 consecutive no-bid pair, so 60 never
  // entered even though it is quoted at 0.05/0.25 -- and 60 is still a listed
  // strike below the stop, so the flag is TRUE.
  EXPECT_TRUE(cut->zero_quote_truncated_low);
  EXPECT_FALSE(cut->zero_quote_truncated_high);
  EXPECT_EQ(cut->n_puts, 2u);
  ASSERT_EQ(cut->terms.size(), 4u);
  const double cut_k[4] = {90.0, 95.0, 100.0, 110.0};
  const double cut_dk[4] = {5.0, 5.0, 7.5, 10.0}; // 95-90, (100-90)/2, (110-95)/2, 110-100
  for (std::size_t i = 0; i < 4u; ++i) {
    EXPECT_DOUBLE_EQ(cut->terms[i].strike, cut_k[i]) << "strike at term " << i;
    EXPECT_DOUBLE_EQ(cut->terms[i].delta_k, cut_dk[i]) << "dK at term " << i;
  }

  // Not truncated: the 65 bid resets the run, so 65 AND 60 are admitted, and the
  // surviving-strip dK at 90 widens from 5 to 15 because its lower neighbour is
  // now 65 rather than 95's own gap.
  EXPECT_FALSE(full->zero_quote_truncated_low);
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

// A LONE excluded strike is skipped but does not stop the walk -- only a
// consecutive pair does.
TEST(CboeStrip, ZeroQuote_SingleGapDoesNotStopTheWalk) {
  const std::vector<CboeStrikeQuote> board = truncation_board(0.10);
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  for (const auto &term : got->terms) {
    EXPECT_NE(term.strike, 80.0) << "the bidless 80 put must be excluded";
  }
  EXPECT_EQ(got->n_puts, 4u); // 60, 65, 90, 95 -- the walk crossed the gap
}

// The truncation flag separates QUOTE QUALITY from LISTING COVERAGE: it is TRUE
// only when listed strikes remained beyond the stop. Here the bidless pair IS the
// bottom of the board, so nothing was refused and the flag stays FALSE -- the
// case that used to be a false positive.
TEST(CboeStrip, ZeroQuote_TruncationAtTheBoardEdgeIsNotAQualitySignal) {
  const std::vector<CboeStrikeQuote> board = {
      row(80.0, 23.00, 23.60, 0.00, 0.30), // bidless, and the lowest listed strike
      row(90.0, 13.90, 14.50, 0.00, 0.30), // bidless -> the pair that stops the walk
      row(95.0, 9.60, 10.10, 1.60, 2.00),
      row(100.0, 6.20, 6.60, 2.40, 2.80),
      row(110.0, 1.90, 2.30, 7.60, 8.00),
  };
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_EQ(got->n_puts, 1u); // only 95 survives
  EXPECT_FALSE(got->zero_quote_truncated_low)
      << "the triggering pair was the end of the board; no listed strike was refused";
}

// The call wing obeys the same rule, keyed on the CALL quote. The board keeps one
// live call so the strip stays calculable -- the empty-wing refusal is
// `NotCalculable_EveryCallExcluded`, not this test.
TEST(CboeStrip, ZeroQuote_TruncationAppliesToTheCallWing) {
  std::vector<CboeStrikeQuote> board = worked_board();
  board.push_back(row(150.0, 0.05, 0.25, 45.00, 45.60)); // quoted, but unreachable
  board.push_back(row(170.0, 0.05, 0.25, 65.00, 65.60)); // ditto
  board[5].call_bid = 0.0;                               // 130 call bidless (run 1)
  board[6].call_bid = 0.0;                               // 150 call bidless (run 2) -> stop

  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_TRUE(got->zero_quote_truncated_high) << "170 remained beyond the stop and was refused";
  EXPECT_EQ(got->n_calls, 1u);        // only 110 survives
  EXPECT_DOUBLE_EQ(got->k_hi, 110.0); // the strip ends at the last live call
  for (const auto &term : got->terms) {
    EXPECT_NE(term.strike, 170.0) << "a quoted strike beyond the stop must be discarded";
  }
}

// [CUR-M] §3(a)(iii) as amended 2025-02-10 excludes on a zero ASK as well as a
// zero bid. A bid-with-no-offer series is a routine deep-wing state: it must
// EXCLUDE that one series, not reject the whole board as a crossed quote.
TEST(CboeStrip, ZeroQuote_ZeroAskExcludesTheSeriesAndKeepsTheBoard) {
  std::vector<CboeStrikeQuote> board = worked_board();
  board[1].put_bid = 0.05; // the 85 put keeps a bid ...
  board[1].put_ask = 0.00; // ... but loses its offer

  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_EQ(got->n_puts, 2u); // 70 and 95; the 85 series is out
  for (const auto &term : got->terms) {
    EXPECT_NE(term.strike, 85.0) << "a zero-ask series must be excluded";
  }
  // The walk crossed the single gap, so the strip's low edge is still 70 and its
  // dK now spans the hole: 95 - 70 = 25 at the endpoint.
  EXPECT_DOUBLE_EQ(got->k_lo, 70.0);
  EXPECT_DOUBLE_EQ(got->terms[0].delta_k, 25.0);
}

TEST(CboeStrip, ZeroQuote_TwoConsecutiveZeroAsksTruncate) {
  std::vector<CboeStrikeQuote> board = worked_board();
  board[1].put_ask = 0.00; // 85 put: bid 0.50, no offer  (run 1)
  board[2].put_ask = 0.00; // 95 put: bid 1.60, no offer  (run 2) -> stop
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  // 70 sits below the stop, so every out-of-the-money put is gone and there is
  // no index -- which is itself the proof that the zero-ASK half truncated.
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::Unavailable);
}

// ── 4. K0 selection at and near F ───────────────────────────────────────────

TEST(CboeStrip, K0Selection_ForwardExactlyOnAStrikeZeroesTheTaylorTerm) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const auto got = cboe_var_strike(board, 100.0, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  // [CUR-M] §3(a): "First strike EQUAL TO or otherwise immediately below F". The
  // 2009 edition and the 2019 symbol table read "first strike below" and are
  // superseded; do not "restore" a strict `<` here.
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

  // The tie is not numerically negligible, which is why the inclusive rule is
  // implemented rather than approximated: one strike of K0 movement is worth
  // several vol points on this board.
  const auto at_strike = cboe_var_strike(board, 100.0, kDf, kT);
  ASSERT_TRUE(at_strike.has_value()) << at_strike.error().to_string();
  EXPECT_GT(std::abs(got->vol_strike_dec - at_strike->vol_strike_dec), 0.04);
}

TEST(CboeStrip, K0Selection_JustAboveAStrikeStaysPut) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const auto got = cboe_var_strike(board, 100.001, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_DOUBLE_EQ(got->k0, 100.0);
  EXPECT_GT(got->taylor_term, -1e-6);
  EXPECT_LT(got->taylor_term, 0.0);
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

// ── 6. The two scalar multipliers ───────────────────────────────────────────

// 1/df multiplies the sum and nothing else, so sum_term(df)*df is invariant and
// the Taylor term does not move at all. A suite whose fixture pinned df to 1.0
// would never notice the e^{RT} factor being dropped.
TEST(CboeStrip, Scaling_DiscountFactorScalesTheSumAndNotTheTaylorTerm) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const auto discounted = cboe_var_strike(board, kF, 0.95, kT);
  ASSERT_TRUE(discounted.has_value()) << discounted.error().to_string();
  const auto undiscounted = cboe_var_strike(board, kF, 1.0, kT);
  ASSERT_TRUE(undiscounted.has_value()) << undiscounted.error().to_string();

  EXPECT_NEAR(discounted->sum_term * 0.95, undiscounted->sum_term, 1e-14);
  EXPECT_DOUBLE_EQ(discounted->taylor_term, undiscounted->taylor_term);
  EXPECT_GT(discounted->var_strike_dec, undiscounted->var_strike_dec);
}

// The same treatment for T, which every other successful call in this file holds
// at 0.25. Both terms carry 1/T, so doubling T halves BOTH -- and the whole
// variance with them. Without this, replacing `2.0/maturity_t` with the constant
// 8.0 and `1.0/maturity_t` with 4.0 leaves the entire suite green: the RELATIVE
// weighting of the two terms is pinned by the worked example, but the ABSOLUTE
// 1/T scaling is pinned nowhere else.
TEST(CboeStrip, Scaling_MaturityScalesBothTerms) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  const auto quarter = cboe_var_strike(board, kF, kDf, 0.25);
  ASSERT_TRUE(quarter.has_value()) << quarter.error().to_string();
  const auto half = cboe_var_strike(board, kF, kDf, 0.50);
  ASSERT_TRUE(half.has_value()) << half.error().to_string();

  EXPECT_NEAR(half->sum_term * 2.0, quarter->sum_term, 1e-14);
  EXPECT_NEAR(half->taylor_term * 2.0, quarter->taylor_term, 1e-16);
  EXPECT_NEAR(half->var_strike_dec * 2.0, quarter->var_strike_dec, 1e-14);
  // The strip itself is a function of the board alone -- T rescales the answer,
  // it does not reselect the strip.
  ASSERT_EQ(half->terms.size(), quarter->terms.size());
  for (std::size_t i = 0; i < half->terms.size(); ++i) {
    EXPECT_DOUBLE_EQ(half->terms[i].contribution, quarter->terms[i].contribution);
  }
}

// ── 7. [CUR-M] §5(b): the board admits no index ─────────────────────────────

TEST(CboeStrip, NotCalculable_EveryPutExcluded) {
  std::vector<CboeStrikeQuote> board = worked_board();
  board[0].put_bid = 0.0; // 70
  board[1].put_bid = 0.0; // 85
  board[2].put_bid = 0.0; // 95
  CboeVarStrip diag{};
  const auto got = cboe_var_strike(board, kF, kDf, kT, &diag);
  ASSERT_FALSE(got.has_value()) << "a one-sided strip is not a cheaper answer";
  EXPECT_EQ(got.error().code(), ErrorCode::Unavailable);
  // The diagnostic still tells the caller how far resolution got.
  EXPECT_DOUBLE_EQ(diag.k0, 100.0);
  EXPECT_EQ(diag.n_puts, 0u);
  EXPECT_EQ(diag.n_calls, 2u);
}

TEST(CboeStrip, NotCalculable_EveryCallExcluded) {
  std::vector<CboeStrikeQuote> board = worked_board();
  board[4].call_bid = 0.0; // 110
  board[5].call_bid = 0.0; // 130
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::Unavailable);
}

// A forward above every listed strike leaves no out-of-the-money call at all.
// This used to return a confident 0.2499 with n_calls == 0.
TEST(CboeStrip, NotCalculable_ForwardAboveTheBoardLeavesNoCallWing) {
  const std::vector<CboeStrikeQuote> board = worked_board();
  CboeVarStrip diag{};
  const auto got = cboe_var_strike(board, 132.0, kDf, kT, &diag);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::Unavailable);
  EXPECT_DOUBLE_EQ(diag.k0, 130.0); // K0 still resolved before the refusal
  EXPECT_EQ(diag.n_calls, 0u);
}

// Every wing bidless: both wings die on their first consecutive pair. Formerly
// this reported InvalidArgument via a structural "fewer than two strikes" guard;
// it is the same board, now refused for the reason Cboe gives.
TEST(CboeStrip, NotCalculable_AllZeroBid) {
  const std::vector<CboeStrikeQuote> board = {
      row(80.0, 0.00, 24.00, 0.00, 0.40), row(90.0, 0.00, 14.50, 0.00, 1.00),
      row(100.0, 6.20, 6.60, 2.40, 2.80), row(110.0, 0.00, 2.30, 0.00, 8.00),
      row(120.0, 0.00, 0.80, 0.00, 16.50),
  };
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::Unavailable);
}

// [CUR-M] §3(a)(ii): "If quotes of the K0 put option or the K0 call option are
// NULL ... then the Cboe volatility index cannot be calculated." An unquoted K0
// call formerly moved Q(K0) from 4.50 to 1.30 and reported 23.17 vol where the
// board supports 27.18 -- silently, with every diagnostic reporting a clean strip.
TEST(CboeStrip, NotCalculable_NullK0CallLeg) {
  std::vector<CboeStrikeQuote> board = worked_board();
  board[3].call_bid = 0.00; // K0 = 100: no market on the call at all
  board[3].call_ask = 0.00;
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::Unavailable);
}

TEST(CboeStrip, NotCalculable_NullK0PutLeg) {
  std::vector<CboeStrikeQuote> board = worked_board();
  board[3].put_bid = 0.00;
  board[3].put_ask = 0.00;
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::Unavailable);
}

// The NARROW reading, recorded so a future change of mind is a deliberate one: a
// ONE-SIDED K0 quote is not a NULL quote. Cboe refuses only "null" at K0, and
// §3(a)(iii)'s zero-bid-or-ask exclusion governs the walk, which does not select
// K0. See cboe_strip.hpp's open-question paragraph.
TEST(CboeStrip, NotCalculable_OneSidedK0LegIsNotNull) {
  std::vector<CboeStrikeQuote> board = worked_board();
  board[3].call_bid = 0.00; // K0 call quoted 0.00 / 6.60 -- a real mid of 3.30
  const auto got = cboe_var_strike(board, kF, kDf, kT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_DOUBLE_EQ(got->terms[3].mid, 0.5 * (2.60 + 3.30));
}

// ── 8. Malformed boards ─────────────────────────────────────────────────────

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

// A genuinely crossed quote -- both sides non-zero, ask below bid. Distinct from
// the bid-with-no-offer case above, which is a legitimate one-sided market.
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
// That is not a mark -- it is an OutOfRange, so no caller ever receives a NaN vol
// from a sqrt of a negative variance. The populated strip still reaches the
// caller through `diagnostic_out`, which is the caller who needs it most.
TEST(CboeStrip, Degenerate_NegativeVarianceIsRejectedButDiagnosed) {
  std::vector<CboeStrikeQuote> board = worked_board();
  board.push_back(row(260.0, 0.05, 0.15, 155.00, 156.00)); // one live call above F
  CboeVarStrip diag{};
  const auto got = cboe_var_strike(board, 200.0, kDf, kT, &diag);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::OutOfRange);
  EXPECT_LT(diag.var_strike_dec, 0.0);
  EXPECT_DOUBLE_EQ(diag.k0, 130.0);
  EXPECT_FALSE(diag.terms.empty()) << "the audit trail must survive the rejection";
  EXPECT_DOUBLE_EQ(diag.var_strike_dec, diag.sum_term + diag.taylor_term);
}

// ── 9. The basis diagnostic ─────────────────────────────────────────────────

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

// ── 10. The cited Cboe worked example ───────────────────────────────────────
//
// See the file header for the full citation, for what this oracle does and does
// not cover, and for why the sigma^2 tolerance is 1e-8 and must not be tightened.

namespace whitepaper {

constexpr CboeStripLeg kP = CboeStripLeg::Put;
constexpr CboeStripLeg kC = CboeStripLeg::Call;
constexpr CboeStripLeg kA = CboeStripLeg::K0Average;

// One published strip row: [CUR-V] Appendix 5 prints K, the option type, the
// midpoint Q(K), dK and the individual contribution. The contribution column is
// NOT reproduced here because it is fully determined by the other three plus
// e^{RT}: asserting every dK and every Q, plus the published strip sum at 1e-10,
// pins each contribution without a second transcription surface to get wrong.
struct StripRow {
  double strike;
  CboeStripLeg leg;
  double q;
  double delta_k;
};

// A published raw two-sided quote ([CUR-V] Appendix 3's exclusion-boundary
// tables). Used both to override the bid == ask == Q default for strip members
// whose raw quotes ARE published, and to place the excluded board rows.
struct QuoteRow {
  double strike;
  CboeStripLeg leg;
  double bid;
  double ask;
};

// A few excluded board rows are published with a zero BID but no ask. A nominal
// non-zero ask is used so the row is a zero-BID exclusion rather than a null
// quote. The choice is provably immaterial: both are excluded by the same rule,
// both increment the same consecutive-exclusion run, and an excluded strike's
// quote never enters the sum.
constexpr double kUnpublishedAsk = 0.05;

constexpr StripRow kNearStrip[] = {
    {1370.0, kP, 0.2, 5.0},    {1375.0, kP, 0.125, 5.0},
    {1380.0, kP, 0.15, 5.0},   {1385.0, kP, 0.225, 5.0},
    {1390.0, kP, 0.225, 5.0},  {1395.0, kP, 0.125, 5.0},
    {1400.0, kP, 0.125, 7.5},  {1410.0, kP, 0.225, 10.0},
    {1420.0, kP, 0.225, 7.5},  {1425.0, kP, 0.175, 5.0},
    {1430.0, kP, 0.225, 5.0},  {1435.0, kP, 0.275, 5.0},
    {1440.0, kP, 0.175, 5.0},  {1445.0, kP, 0.225, 5.0},
    {1450.0, kP, 0.2, 5.0},    {1455.0, kP, 0.25, 5.0},
    {1460.0, kP, 0.25, 5.0},   {1465.0, kP, 0.25, 5.0},
    {1470.0, kP, 0.25, 5.0},   {1475.0, kP, 0.2, 5.0},
    {1480.0, kP, 0.25, 5.0},   {1485.0, kP, 0.35, 5.0},
    {1490.0, kP, 0.175, 5.0},  {1495.0, kP, 0.275, 5.0},
    {1500.0, kP, 0.325, 5.0},  {1505.0, kP, 0.325, 5.0},
    {1510.0, kP, 0.3, 5.0},    {1515.0, kP, 0.3, 5.0},
    {1520.0, kP, 0.35, 5.0},   {1525.0, kP, 0.35, 5.0},
    {1530.0, kP, 0.325, 5.0},  {1535.0, kP, 0.375, 5.0},
    {1540.0, kP, 0.375, 5.0},  {1545.0, kP, 0.375, 5.0},
    {1550.0, kP, 0.5, 5.0},    {1555.0, kP, 0.425, 5.0},
    {1560.0, kP, 0.425, 5.0},  {1565.0, kP, 0.425, 5.0},
    {1570.0, kP, 0.475, 5.0},  {1575.0, kP, 0.55, 5.0},
    {1580.0, kP, 0.525, 5.0},  {1585.0, kP, 0.525, 5.0},
    {1590.0, kP, 0.525, 5.0},  {1595.0, kP, 0.525, 5.0},
    {1600.0, kP, 0.675, 5.0},  {1605.0, kP, 0.575, 5.0},
    {1610.0, kP, 0.625, 5.0},  {1615.0, kP, 0.625, 5.0},
    {1620.0, kP, 0.625, 5.0},  {1625.0, kP, 0.675, 5.0},
    {1630.0, kP, 0.675, 5.0},  {1635.0, kP, 0.725, 5.0},
    {1640.0, kP, 0.725, 5.0},  {1645.0, kP, 0.775, 5.0},
    {1650.0, kP, 0.675, 5.0},  {1655.0, kP, 0.825, 5.0},
    {1660.0, kP, 0.825, 5.0},  {1665.0, kP, 0.875, 5.0},
    {1670.0, kP, 0.875, 5.0},  {1675.0, kP, 0.925, 5.0},
    {1680.0, kP, 0.975, 5.0},  {1685.0, kP, 1.025, 5.0},
    {1690.0, kP, 1.025, 5.0},  {1695.0, kP, 1.075, 5.0},
    {1700.0, kP, 1.125, 5.0},  {1705.0, kP, 1.125, 5.0},
    {1710.0, kP, 1.175, 5.0},  {1715.0, kP, 1.225, 5.0},
    {1720.0, kP, 1.275, 5.0},  {1725.0, kP, 1.325, 5.0},
    {1730.0, kP, 1.375, 5.0},  {1735.0, kP, 1.425, 5.0},
    {1740.0, kP, 1.475, 5.0},  {1745.0, kP, 1.55, 5.0},
    {1750.0, kP, 1.6, 5.0},    {1755.0, kP, 1.675, 5.0},
    {1760.0, kP, 1.75, 5.0},   {1765.0, kP, 1.825, 5.0},
    {1770.0, kP, 1.9, 5.0},    {1775.0, kP, 2.0, 5.0},
    {1780.0, kP, 2.075, 5.0},  {1785.0, kP, 2.175, 5.0},
    {1790.0, kP, 2.25, 5.0},   {1795.0, kP, 2.375, 5.0},
    {1800.0, kP, 2.525, 5.0},  {1805.0, kP, 2.625, 5.0},
    {1810.0, kP, 2.775, 5.0},  {1815.0, kP, 2.95, 5.0},
    {1820.0, kP, 3.075, 5.0},  {1825.0, kP, 3.3, 5.0},
    {1830.0, kP, 3.45, 5.0},   {1835.0, kP, 3.65, 5.0},
    {1840.0, kP, 3.9, 5.0},    {1845.0, kP, 4.1, 5.0},
    {1850.0, kP, 4.35, 5.0},   {1855.0, kP, 4.65, 5.0},
    {1860.0, kP, 4.95, 5.0},   {1865.0, kP, 5.25, 5.0},
    {1870.0, kP, 5.6, 5.0},    {1875.0, kP, 6.0, 5.0},
    {1880.0, kP, 6.4, 5.0},    {1885.0, kP, 6.85, 5.0},
    {1890.0, kP, 7.35, 5.0},   {1895.0, kP, 7.9, 5.0},
    {1900.0, kP, 8.3, 5.0},    {1905.0, kP, 9.0, 5.0},
    {1910.0, kP, 9.65, 5.0},   {1915.0, kP, 10.6, 5.0},
    {1920.0, kP, 11.4, 5.0},   {1925.0, kP, 12.1, 5.0},
    {1930.0, kP, 13.25, 5.0},  {1935.0, kP, 14.15, 5.0},
    {1940.0, kP, 15.25, 5.0},  {1945.0, kP, 16.55, 5.0},
    {1950.0, kP, 18.25, 5.0},  {1955.0, kP, 19.75, 5.0},
    {1960.0, kA, 22.775, 5.0}, {1965.0, kC, 21.05, 5.0},
    {1970.0, kC, 18.1, 5.0},   {1975.0, kC, 15.25, 5.0},
    {1980.0, kC, 12.75, 5.0},  {1985.0, kC, 10.45, 5.0},
    {1990.0, kC, 8.45, 5.0},   {1995.0, kC, 6.65, 5.0},
    {2000.0, kC, 4.95, 5.0},   {2005.0, kC, 3.8, 5.0},
    {2010.0, kC, 2.875, 5.0},  {2015.0, kC, 2.025, 5.0},
    {2020.0, kC, 1.45, 5.0},   {2025.0, kC, 1.125, 5.0},
    {2030.0, kC, 0.725, 5.0},  {2035.0, kC, 0.525, 5.0},
    {2040.0, kC, 0.5, 5.0},    {2045.0, kC, 0.4, 5.0},
    {2050.0, kC, 0.25, 5.0},   {2055.0, kC, 0.325, 5.0},
    {2060.0, kC, 0.225, 5.0},  {2065.0, kC, 0.175, 5.0},
    {2070.0, kC, 0.15, 5.0},   {2075.0, kC, 0.15, 5.0},
    {2080.0, kC, 0.25, 5.0},   {2085.0, kC, 0.225, 5.0},
    {2090.0, kC, 0.1, 5.0},    {2095.0, kC, 0.2, 5.0},
    {2100.0, kC, 0.1, 15.0},   {2125.0, kC, 0.1, 25.0},
};

constexpr StripRow kNextStrip[] = {
    {1275.0, kP, 0.075, 50.0}, {1325.0, kP, 0.15, 37.5},
    {1350.0, kP, 0.15, 25.0},  {1375.0, kP, 0.175, 25.0},
    {1400.0, kP, 0.2, 25.0},   {1425.0, kP, 0.25, 25.0},
    {1450.0, kP, 0.3, 25.0},   {1475.0, kP, 0.35, 25.0},
    {1500.0, kP, 0.4, 17.5},   {1510.0, kP, 0.425, 10.0},
    {1520.0, kP, 0.45, 7.5},   {1525.0, kP, 0.475, 5.0},
    {1530.0, kP, 0.5, 7.5},    {1540.0, kP, 0.525, 10.0},
    {1550.0, kP, 0.55, 7.5},   {1555.0, kP, 0.575, 5.0},
    {1560.0, kP, 0.6, 5.0},    {1565.0, kP, 0.625, 5.0},
    {1570.0, kP, 0.65, 5.0},   {1575.0, kP, 0.675, 5.0},
    {1580.0, kP, 0.675, 5.0},  {1585.0, kP, 0.7, 5.0},
    {1590.0, kP, 0.725, 5.0},  {1595.0, kP, 0.75, 5.0},
    {1600.0, kP, 0.775, 5.0},  {1605.0, kP, 0.8, 5.0},
    {1610.0, kP, 0.825, 5.0},  {1615.0, kP, 0.85, 5.0},
    {1620.0, kP, 0.875, 5.0},  {1625.0, kP, 0.9, 5.0},
    {1630.0, kP, 0.95, 5.0},   {1635.0, kP, 0.975, 5.0},
    {1640.0, kP, 1.0, 5.0},    {1645.0, kP, 1.025, 5.0},
    {1650.0, kP, 1.075, 5.0},  {1655.0, kP, 1.1, 5.0},
    {1660.0, kP, 1.15, 5.0},   {1665.0, kP, 1.2, 5.0},
    {1670.0, kP, 1.225, 5.0},  {1675.0, kP, 1.275, 5.0},
    {1680.0, kP, 1.325, 5.0},  {1685.0, kP, 1.375, 5.0},
    {1690.0, kP, 1.425, 5.0},  {1695.0, kP, 1.475, 5.0},
    {1700.0, kP, 1.525, 5.0},  {1705.0, kP, 1.6, 5.0},
    {1710.0, kP, 1.675, 5.0},  {1715.0, kP, 1.725, 5.0},
    {1720.0, kP, 1.8, 5.0},    {1725.0, kP, 1.85, 5.0},
    {1730.0, kP, 1.925, 5.0},  {1735.0, kP, 2.0, 5.0},
    {1740.0, kP, 2.1, 5.0},    {1745.0, kP, 2.175, 5.0},
    {1750.0, kP, 2.275, 5.0},  {1755.0, kP, 2.375, 5.0},
    {1760.0, kP, 2.475, 5.0},  {1765.0, kP, 2.575, 5.0},
    {1770.0, kP, 2.725, 5.0},  {1775.0, kP, 2.825, 5.0},
    {1780.0, kP, 3.0, 5.0},    {1785.0, kP, 3.1, 5.0},
    {1790.0, kP, 3.25, 5.0},   {1795.0, kP, 3.45, 5.0},
    {1800.0, kP, 3.6, 5.0},    {1805.0, kP, 3.8, 5.0},
    {1810.0, kP, 3.95, 5.0},   {1815.0, kP, 4.2, 5.0},
    {1820.0, kP, 4.4, 5.0},    {1825.0, kP, 4.65, 5.0},
    {1830.0, kP, 4.9, 5.0},    {1835.0, kP, 5.15, 5.0},
    {1840.0, kP, 5.45, 5.0},   {1845.0, kP, 5.75, 5.0},
    {1850.0, kP, 6.05, 5.0},   {1855.0, kP, 6.45, 5.0},
    {1860.0, kP, 6.75, 5.0},   {1865.0, kP, 7.15, 5.0},
    {1870.0, kP, 7.65, 5.0},   {1875.0, kP, 8.15, 5.0},
    {1880.0, kP, 8.6, 5.0},    {1885.0, kP, 9.2, 5.0},
    {1890.0, kP, 9.75, 5.0},   {1895.0, kP, 10.4, 5.0},
    {1900.0, kP, 11.1, 5.0},   {1905.0, kP, 11.8, 5.0},
    {1910.0, kP, 12.6, 5.0},   {1915.0, kP, 13.45, 5.0},
    {1920.0, kP, 14.4, 5.0},   {1925.0, kP, 15.4, 5.0},
    {1930.0, kP, 16.4, 5.0},   {1935.0, kP, 17.6, 5.0},
    {1940.0, kP, 18.8, 5.0},   {1945.0, kP, 20.2, 5.0},
    {1950.0, kP, 21.6, 5.0},   {1955.0, kP, 23.2, 5.0},
    {1960.0, kA, 26.1, 5.0},   {1965.0, kC, 24.15, 5.0},
    {1970.0, kC, 21.1, 5.0},   {1975.0, kC, 18.3, 5.0},
    {1980.0, kC, 15.7, 5.0},   {1985.0, kC, 13.3, 5.0},
    {1990.0, kC, 11.1, 5.0},   {1995.0, kC, 9.15, 5.0},
    {2000.0, kC, 7.4, 5.0},    {2005.0, kC, 5.85, 5.0},
    {2010.0, kC, 4.65, 5.0},   {2015.0, kC, 3.55, 5.0},
    {2020.0, kC, 2.7, 5.0},    {2025.0, kC, 2.05, 5.0},
    {2030.0, kC, 1.55, 5.0},   {2035.0, kC, 1.15, 5.0},
    {2040.0, kC, 0.875, 5.0},  {2045.0, kC, 0.675, 5.0},
    {2050.0, kC, 0.575, 7.5},  {2060.0, kC, 0.35, 10.0},
    {2070.0, kC, 0.25, 7.5},   {2075.0, kC, 0.2, 15.0},
    {2100.0, kC, 0.15, 25.0},  {2125.0, kC, 0.1, 25.0},
    {2150.0, kC, 0.1, 37.5},   {2200.0, kC, 0.075, 50.0},
};

// Raw published quotes for strip members ([CUR-V] Appendix 3). Their midpoints
// must reproduce the Appendix-5 Q(K) above -- asserted, not assumed.
constexpr QuoteRow kNearRawInStrip[] = {
    {1370.0, kP, 0.05, 0.35}, {1375.0, kP, 0.10, 0.15}, {1380.0, kP, 0.10, 0.20},
    {2095.0, kC, 0.05, 0.35}, {2100.0, kC, 0.05, 0.15}, {2125.0, kC, 0.05, 0.15},
};

// Board rows the selection rules must EXCLUDE. [CUR-V] Appendix 3's boundary
// tables, plus the four interior zero-bid strikes (near 1405/1415/2120, next
// 1300) whose exclusion is what makes the published dK column discriminate
// surviving-strike from board-strike derivation.
constexpr QuoteRow kNearExcluded[] = {
    {1345.0, kP, 0.00, 0.15},            // zero bid, beyond the truncation
    {1350.0, kP, 0.05, 0.15},            // HAS a bid, still excluded
    {1355.0, kP, 0.05, 0.35},            // HAS a bid, still excluded
    {1360.0, kP, 0.00, 0.35},            // truncating pair
    {1365.0, kP, 0.00, 0.35},            // truncating pair
    {1405.0, kP, 0.00, kUnpublishedAsk}, // interior exclusion
    {1415.0, kP, 0.00, kUnpublishedAsk}, // interior exclusion
    {2120.0, kC, 0.00, 0.15},            // lone zero bid: excluded, no truncation
    {2150.0, kC, 0.00, 0.10},            // truncating pair
    {2175.0, kC, 0.00, 0.05},            // truncating pair
    {2200.0, kC, 0.00, 0.05},            // beyond the truncation
    {2225.0, kC, 0.05, 0.10},            // HAS a bid, still excluded
    {2250.0, kC, 0.00, 0.05},            // beyond the truncation
};

constexpr QuoteRow kNextExcluded[] = {
    {1225.0, kP, 0.00, kUnpublishedAsk}, // truncating pair
    {1250.0, kP, 0.00, kUnpublishedAsk}, // truncating pair
    {1300.0, kP, 0.00, kUnpublishedAsk}, // interior exclusion
};

// [CUR-V] Appendix 3, verbatim. T is the published year-fraction (minutes to
// expiry / 525,600); the raw minute counts are carried separately because the
// 30-day index weights use them directly.
constexpr double kNearT = 0.0656088; // 34,484 minutes
constexpr double kNextT = 0.0855289; // 44,954 minutes
constexpr double kNearR = 0.00031664;
constexpr double kNextR = 0.00028797;
constexpr double kNearF = 1962.89996;
constexpr double kNextF = 1962.40006;
constexpr double kK0 = 1960.0;
// "The price used for the 1960 strike in the near-term is, therefore,
//  (24.25 + 21.30)/2 = 22.775. ... in the next term is (27.30 + 24.90)/2 = 26.10."
constexpr double kNearK0Call = 24.25;
constexpr double kNearK0Put = 21.30;
constexpr double kNextK0Call = 27.30;
constexpr double kNextK0Put = 24.90;

// [CUR-V] Appendix 5, foot of page 23, and Appendix 3, page 13.
constexpr double kNearPublishedSum = 0.0006320516;
constexpr double kNextPublishedSum = 0.0008314016;
constexpr double kNearPublishedScaledSum = 0.019267; // printed to 6 dp
constexpr double kNextPublishedScaledSum = 0.019441; // printed to 6 dp
constexpr double kNearPublishedTaylor = 0.00003337;
constexpr double kNextPublishedTaylor = 0.00001753;
constexpr double kNearPublishedSigma2 = 0.019233906;
constexpr double kNextPublishedSigma2 = 0.019423884;

[[nodiscard]] std::vector<CboeStrikeQuote> build_board(std::span<const StripRow> strip,
                                                       std::span<const QuoteRow> raw_in_strip,
                                                       std::span<const QuoteRow> excluded,
                                                       double k0_put, double k0_call) {
  std::map<double, CboeStrikeQuote> rows; // ascending by strike, which the API requires
  const auto at = [&rows](double strike) -> CboeStrikeQuote & {
    CboeStrikeQuote &r = rows[strike];
    r.strike = strike;
    return r;
  };
  const auto set_leg = [](CboeStrikeQuote &r, CboeStripLeg leg, double bid, double ask) {
    if (leg == kP) {
      r.put_bid = bid;
      r.put_ask = ask;
    } else {
      r.call_bid = bid;
      r.call_ask = ask;
    }
  };
  for (const StripRow &s : strip) {
    CboeStrikeQuote &r = at(s.strike);
    if (s.leg == kA) {
      // The one strike carrying both legs. Appendix 3 publishes these two mid
      // quotes; Appendix 5's Q(1960) is their average, which the test asserts.
      set_leg(r, kP, k0_put, k0_put);
      set_leg(r, kC, k0_call, k0_call);
    } else {
      // Appendix 5 publishes the MIDPOINT, not the raw quote -- see the file
      // header. bid == ask == Q reproduces that midpoint exactly.
      set_leg(r, s.leg, s.q, s.q);
    }
  }
  for (const QuoteRow &q : raw_in_strip) {
    set_leg(at(q.strike), q.leg, q.bid, q.ask);
  }
  for (const QuoteRow &q : excluded) {
    set_leg(at(q.strike), q.leg, q.bid, q.ask);
  }
  std::vector<CboeStrikeQuote> board;
  board.reserve(rows.size());
  for (const auto &entry : rows) {
    board.push_back(entry.second);
  }
  return board;
}

// NaN when the strike is not in the strip, so a caller can assert either the dK
// or the absence with the same probe.
[[nodiscard]] double delta_k_at(const CboeVarStrip &strip, double strike) {
  for (const auto &term : strip.terms) {
    if (term.strike == strike) {
      return term.delta_k;
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

// Assert one expiry against its published strip, then hand back sigma^2 so the
// index composition below can close the example out.
void check_expiry(std::span<const StripRow> strip, std::span<const QuoteRow> raw_in_strip,
                  std::span<const QuoteRow> excluded, double k0_put, double k0_call, double forward,
                  double rate, double maturity_t, double published_sum,
                  double published_scaled_sum, double published_taylor, double published_sigma2,
                  bool expect_truncated_low, bool expect_truncated_high, const char *label) {
  const std::vector<CboeStrikeQuote> board =
      build_board(strip, raw_in_strip, excluded, k0_put, k0_call);
  const double df = std::exp(-rate * maturity_t);
  const auto got = cboe_var_strike(board, forward, df, maturity_t);
  ASSERT_TRUE(got.has_value()) << label << ": " << got.error().to_string();

  EXPECT_DOUBLE_EQ(got->k0, kK0) << label;
  ASSERT_EQ(got->terms.size(), strip.size()) << label << ": strip membership";

  // Every published strike, leg, midpoint and dK. The dK column is the
  // load-bearing one: six of these strikes have a board neighbour that differs
  // from their strip neighbour, so a board-derived dK fails here and only here.
  for (std::size_t i = 0; i < strip.size(); ++i) {
    EXPECT_DOUBLE_EQ(got->terms[i].strike, strip[i].strike) << label << " strike @" << i;
    EXPECT_EQ(got->terms[i].leg, strip[i].leg) << label << " leg @" << i;
    EXPECT_DOUBLE_EQ(got->terms[i].mid, strip[i].q) << label << " Q(K) @" << i;
    EXPECT_DOUBLE_EQ(got->terms[i].delta_k, strip[i].delta_k) << label << " dK @" << i;
  }

  double sum = 0.0;
  for (const auto &term : got->terms) {
    sum += term.contribution;
  }
  // Appendix 5 prints the strip sum to 10 dp, so this one IS asserted tightly.
  EXPECT_NEAR(sum, published_sum, 1e-10) << label << ": strip sum";
  // The (2/T)*Sum line is printed to 6 dp, so half a unit in the last printed
  // place is the most that can be demanded of it.
  EXPECT_NEAR(got->sum_term, published_scaled_sum, 5e-7) << label << ": (2/T)*Sum";
  EXPECT_NEAR(-got->taylor_term, published_taylor, 1e-8) << label << ": Taylor term";
  // 1e-8 and NOT tighter -- see the file header. This is Cboe's display rounding,
  // not this library's accuracy.
  EXPECT_NEAR(got->var_strike_dec, published_sigma2, 1e-8) << label << ": sigma^2";

  EXPECT_EQ(got->zero_quote_truncated_low, expect_truncated_low) << label;
  EXPECT_EQ(got->zero_quote_truncated_high, expect_truncated_high) << label;
}

} // namespace whitepaper

TEST(CboeStrip, WhitePaper_NearTermStrip) {
  using namespace whitepaper;
  // Truncated on both wings: the near-term board carries 1355/1350/1345 below the
  // 1365/1360 stop and 2200/2225/2250 above the 2150/2175 stop, so listed strikes
  // really were refused.
  check_expiry(kNearStrip, kNearRawInStrip, kNearExcluded, kNearK0Put, kNearK0Call, kNearF, kNearR,
               kNearT, kNearPublishedSum, kNearPublishedScaledSum, kNearPublishedTaylor,
               kNearPublishedSigma2, true, true, "near");
}

TEST(CboeStrip, WhitePaper_NextTermStrip) {
  using namespace whitepaper;
  // NOT truncated: the transcribed next-term board stops at 1225, which is the
  // lower triggering strike itself, so nothing was refused below it; and the call
  // wing simply runs out at 2200. Both flags therefore report LISTING COVERAGE
  // rather than quote quality -- exactly the distinction the flag exists to make.
  // This reflects the transcribed board, not a claim about Cboe's full board.
  check_expiry(kNextStrip, {}, kNextExcluded, kNextK0Put, kNextK0Call, kNextF, kNextR, kNextT,
               kNextPublishedSum, kNextPublishedScaledSum, kNextPublishedTaylor,
               kNextPublishedSigma2, false, false, "next");
}

// The six strikes whose BOARD neighbour differs from their STRIP neighbour. If dK
// were derived from the board these would read 5, 5, 5, 12.5, 25 and 25. Called
// out separately from the bulk loop above because they are the single
// highest-value assertions in the whole oracle: every other strike in the example
// is insensitive to the distinction.
TEST(CboeStrip, WhitePaper_DeltaKUsesSurvivingStrikesNotBoardStrikes) {
  using namespace whitepaper;
  const std::vector<CboeStrikeQuote> near_board =
      build_board(kNearStrip, kNearRawInStrip, kNearExcluded, kNearK0Put, kNearK0Call);
  const auto near = cboe_var_strike(near_board, kNearF, std::exp(-kNearR * kNearT), kNearT);
  ASSERT_TRUE(near.has_value()) << near.error().to_string();
  EXPECT_DOUBLE_EQ(delta_k_at(*near, 1400.0), 7.5);  // (1410-1395)/2, skipping 1405
  EXPECT_DOUBLE_EQ(delta_k_at(*near, 1410.0), 10.0); // (1420-1400)/2, skipping 1405 and 1415
  EXPECT_DOUBLE_EQ(delta_k_at(*near, 1420.0), 7.5);  // (1425-1410)/2, skipping 1415
  EXPECT_DOUBLE_EQ(delta_k_at(*near, 2100.0), 15.0); // (2125-2095)/2, skipping 2120

  const std::vector<CboeStrikeQuote> next_board =
      build_board(kNextStrip, {}, kNextExcluded, kNextK0Put, kNextK0Call);
  const auto next = cboe_var_strike(next_board, kNextF, std::exp(-kNextR * kNextT), kNextT);
  ASSERT_TRUE(next.has_value()) << next.error().to_string();
  EXPECT_DOUBLE_EQ(delta_k_at(*next, 1275.0), 50.0); // 1325-1275 endpoint, skipping 1300
  // The paper's own worked dK example: "the dK for the next-term 1325 Put is
  // 37.5: dK = (1350 - 1275)/2", with 1300 on the board and excluded.
  EXPECT_DOUBLE_EQ(delta_k_at(*next, 1325.0), 37.5);

  // And the strikes that must be absent from each strip entirely.
  for (const double excluded : {1405.0, 1415.0, 2120.0}) {
    EXPECT_TRUE(std::isnan(delta_k_at(*near, excluded)))
        << "near strip must not carry " << excluded;
  }
  EXPECT_TRUE(std::isnan(delta_k_at(*next, 1300.0))) << "next strip must not carry 1300";
}

// [CUR-V] Appendix 3's own annotations: strikes beyond a truncation are dropped
// even when they carry a live bid.
TEST(CboeStrip, WhitePaper_QuotedStrikesBeyondTheTruncationAreDropped) {
  using namespace whitepaper;
  const std::vector<CboeStrikeQuote> board =
      build_board(kNearStrip, kNearRawInStrip, kNearExcluded, kNearK0Put, kNearK0Call);
  const auto got = cboe_var_strike(board, kNearF, std::exp(-kNearR * kNearT), kNearT);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  for (const auto &term : got->terms) {
    // 1350 (bid 0.05) and 1355 (bid 0.05) sit below the 1365/1360 stop; 2225
    // (bid 0.05) sits above the 2150/2175 stop. Cboe names all three.
    EXPECT_NE(term.strike, 1350.0);
    EXPECT_NE(term.strike, 1355.0);
    EXPECT_NE(term.strike, 2225.0);
    // 2120 is a LONE zero bid: excluded, but it does not truncate -- 2125 is in.
    EXPECT_NE(term.strike, 2120.0);
  }
  EXPECT_DOUBLE_EQ(got->k_lo, 1370.0);
  EXPECT_DOUBLE_EQ(got->k_hi, 2125.0);
}

// Closes the published example out. The 30-day interpolation is index
// construction and is NOT what this module does -- the test performs it here, on
// the module's two sigma^2 values, purely to show they are right to the precision
// the published index needs. [CUR-V] Appendix 3, page 14.
TEST(CboeStrip, WhitePaper_ComposesToThePublishedVixLevel) {
  using namespace whitepaper;
  const std::vector<CboeStrikeQuote> near_board =
      build_board(kNearStrip, kNearRawInStrip, kNearExcluded, kNearK0Put, kNearK0Call);
  const std::vector<CboeStrikeQuote> next_board =
      build_board(kNextStrip, {}, kNextExcluded, kNextK0Put, kNextK0Call);
  const auto near = cboe_var_strike(near_board, kNearF, std::exp(-kNearR * kNearT), kNearT);
  ASSERT_TRUE(near.has_value()) << near.error().to_string();
  const auto next = cboe_var_strike(next_board, kNextF, std::exp(-kNextR * kNextT), kNextT);
  ASSERT_TRUE(next.has_value()) << next.error().to_string();

  constexpr double kNearMinutes = 34484.0;
  constexpr double kNextMinutes = 44954.0;
  constexpr double kMinutes30Day = 43200.0;
  constexpr double kMinutesYear = 525600.0;
  const double span = kNextMinutes - kNearMinutes;
  const double w_near = (kNextMinutes - kMinutes30Day) / span;
  const double w_next = (kMinutes30Day - kNearMinutes) / span;
  const double inner = (kNearT * near->var_strike_dec * w_near     //
                        + kNextT * next->var_strike_dec * w_next)  //
                       * (kMinutesYear / kMinutes30Day);
  const double vix = 100.0 * std::sqrt(inner);

  EXPECT_NEAR(std::sqrt(inner), 0.13927842, 1e-7); // published inner value
  EXPECT_NEAR(vix, 13.93, 5e-3);                   // published VIX = 13.93
}
