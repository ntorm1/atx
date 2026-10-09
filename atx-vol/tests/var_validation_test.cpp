#include "analytics/var_validation.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace atx::vol;

// ---------------------------------------------------------------------------
// Kupiec proportion-of-failures test.
// ---------------------------------------------------------------------------

// Reference point from the task brief, independently cross-checked against
// scipy.stats.chi2.sf(LR, 1) in a throwaway python session (see
// task-5-report.md for the transcript): n=250, x=5, p=1-0.99=0.01 ->
// LR_pof = 1.9568097882..., p_value = 0.1618549172... . Brief tolerance is
// 1e-3.
TEST(VarValidation, KupiecMatchesHandDerivedReferenceValue) {
  const auto result = kupiec_pof(250u, 5u, 0.99);
  ASSERT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  EXPECT_NEAR(result->lr_pof, 1.9568, 1e-3);
  EXPECT_NEAR(result->p_value, 0.1618, 1e-3);
  EXPECT_EQ(result->n_obs, 250u);
  EXPECT_EQ(result->n_breaches, 5u);
}

// x == 0 (zero breaches): the alternative hypothesis's x*ln(x/n) term is a
// 0*ln(0) limit (== 0 by the header's documented convention), not a NaN.
TEST(VarValidation, KupiecHandlesZeroBreachesByLimit) {
  const auto result = kupiec_pof(100u, 0u, 0.99);
  ASSERT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  EXPECT_TRUE(std::isfinite(result->lr_pof));
  EXPECT_GE(result->lr_pof, 0.0);
  EXPECT_TRUE(std::isfinite(result->p_value));
  EXPECT_GE(result->p_value, 0.0);
  EXPECT_LE(result->p_value, 1.0);
}

// x == n (every observation breaches): the alternative hypothesis's
// (n-x)*ln(1-x/n) term is the same 0*ln(0) limit on the other side.
TEST(VarValidation, KupiecHandlesAllBreachesByLimit) {
  const auto result = kupiec_pof(20u, 20u, 0.99);
  ASSERT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  EXPECT_TRUE(std::isfinite(result->lr_pof));
  EXPECT_GE(result->lr_pof, 0.0);
  EXPECT_TRUE(std::isfinite(result->p_value));
}

TEST(VarValidation, KupiecRejectsInvalidInputs) {
  EXPECT_FALSE(kupiec_pof(0u, 0u, 0.99));   // no observations at all
  EXPECT_FALSE(kupiec_pof(10u, 11u, 0.99)); // more breaches than observations
  EXPECT_FALSE(kupiec_pof(10u, 5u, 0.0));   // confidence must be in (0, 1)
  EXPECT_FALSE(kupiec_pof(10u, 5u, 1.0));
  EXPECT_FALSE(kupiec_pof(10u, 5u, -0.1));
  EXPECT_FALSE(kupiec_pof(10u, 5u, std::numeric_limits<double>::quiet_NaN()));
}

// ---------------------------------------------------------------------------
// Christoffersen independence / conditional-coverage tests.
// ---------------------------------------------------------------------------

// The task brief's prescribed 10-observation sequence, with breach = true:
//   index:    0     1     2     3     4     5     6     7     8     9
//   value:    0     0     1     1     0     0     0     1     0     0
//
// The brief states transition counts (n00,n01,n10,n11) = (5,2,2,0), but
// hand-enumerating the 9 consecutive pairs gives a different result -- and
// the brief explicitly requires hand-deriving and cross-checking numerically
// before pinning, rather than trusting the stated counts blindly:
//   (s0,s1)=(0,0) n00   (s1,s2)=(0,1) n01   (s2,s3)=(1,1) n11
//   (s3,s4)=(1,0) n10   (s4,s5)=(0,0) n00   (s5,s6)=(0,0) n00
//   (s6,s7)=(0,1) n01   (s7,s8)=(1,0) n10   (s8,s9)=(0,0) n00
// which tallies n00=4, n01=2, n10=2, n11=1 (sums to 9, matches n_obs-1).
//
// With those counts: pi01 = n01/(n00+n01) = 2/6 = 1/3, pi11 = n11/(n10+n11)
// = 1/3, pi = (n01+n11)/9 = 3/9 = 1/3 -- pi01, pi11, and pi all coincide
// exactly, so the restricted and unrestricted log-likelihoods are identical
// and LR_independence is exactly 0 (not a "no breaches" or "all breaches"
// degenerate case -- it is the general formula landing on 0 because this
// particular sequence's within-state breach rates happen to equal its
// overall breach rate). Cross-checked against
// scipy.stats.chi2.sf(0.0, 1) == 1.0 and erfc(0) == 1.0 in the same
// throwaway python session as the Kupiec reference value above.
TEST(VarValidation, ChristoffersenIndependenceOnPrescribedSequenceIsExactlyZero) {
  const std::array<bool, 10> sequence{false, false, true, true,  false,
                                      false, false, true, false, false};
  const auto result = christoffersen(std::span<const bool>(sequence), 0.99);
  ASSERT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  EXPECT_NEAR(result->lr_independence, 0.0, 1e-9);
  EXPECT_NEAR(result->p_independence, 1.0, 1e-9);
}

// Conditional coverage for the same sequence at confidence=0.5 (p=0.5,
// n_obs=10, n_breaches=3): kupiec_pof(10, 3, 0.5) independently gives
// LR_pof = 1.645657570101033 (scipy chi2.sf(LR_pof, 1) == 0.1994636115...,
// not used here since christoffersen reports the 2-dof conditional-coverage
// p-value, not the 1-dof POF p-value on its own). Since LR_independence is
// exactly 0 on this sequence, conditional coverage reduces to LR_pof alone:
// LR_cc = 1.645657570101033, p_cc = exp(-LR_cc/2) = 0.439187528538055,
// cross-checked against scipy.stats.chi2.sf(LR_cc, 2).
TEST(VarValidation, ChristoffersenConditionalCoverageMatchesKupiecPlusIndependence) {
  const std::array<bool, 10> sequence{false, false, true, true,  false,
                                      false, false, true, false, false};
  const auto result = christoffersen(std::span<const bool>(sequence), 0.5);
  ASSERT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  const auto direct_kupiec = kupiec_pof(10u, 3u, 0.5);
  ASSERT_TRUE(direct_kupiec);
  // Internal consistency: conditional coverage is exactly POF + independence,
  // whatever floating-point value each component landed on.
  EXPECT_DOUBLE_EQ(result->lr_conditional_coverage,
                   direct_kupiec->lr_pof + result->lr_independence);
  // And the hand-derived/scipy-cross-checked numeric pin.
  EXPECT_NEAR(result->lr_conditional_coverage, 1.645657570101033, 1e-3);
  EXPECT_NEAR(result->p_conditional_coverage, 0.439187528538055, 1e-3);
}

// A second, richer sequence with real clustering (three separate breach runs
// of length 3, 0, ... ) so at least one hard-pinned Christoffersen test in
// this suite has a nonzero, non-degenerate independence statistic:
//   1,1,1,0,0,0,1,1,1,0 -> transition counts n00=2,n01=1,n10=2,n11=4.
// pi01=1/3, pi11=4/6=2/3, pi=5/9 -- these disagree, so LR_independence > 0.
// Hand-computed and cross-checked against scipy.stats.chi2.sf(LR,1) in the
// same throwaway python session: LR_independence = 0.9080533494451899,
// p_independence = 0.3406314504912683.
TEST(VarValidation, ChristoffersenIndependenceOnClusteredSequenceIsNonzero) {
  const std::array<bool, 10> sequence{true,  true, true, false, false,
                                      false, true, true, true,  false};
  const auto result = christoffersen(std::span<const bool>(sequence), 0.99);
  ASSERT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  EXPECT_NEAR(result->lr_independence, 0.9080533494451899, 1e-6);
  EXPECT_NEAR(result->p_independence, 0.3406314504912683, 1e-6);
}

// Degenerate case: zero breaches at all (n01 == n11 == 0). Every
// within-state term short-circuits to 0 before dividing by anything, so
// LR_independence is exactly 0.0, not NaN.
TEST(VarValidation, ChristoffersenHandlesNoBreachesByLimit) {
  const std::array<bool, 5> sequence{false, false, false, false, false};
  const auto result = christoffersen(std::span<const bool>(sequence), 0.99);
  ASSERT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  EXPECT_DOUBLE_EQ(result->lr_independence, 0.0);
  EXPECT_DOUBLE_EQ(result->p_independence, 1.0);
}

// Degenerate case: every observation breaches (n00 == n01 == 0).
TEST(VarValidation, ChristoffersenHandlesAllBreachesByLimit) {
  const std::array<bool, 5> sequence{true, true, true, true, true};
  const auto result = christoffersen(std::span<const bool>(sequence), 0.99);
  ASSERT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  EXPECT_DOUBLE_EQ(result->lr_independence, 0.0);
  EXPECT_DOUBLE_EQ(result->p_independence, 1.0);
}

// Degenerate case explicitly named in the brief: n01 + n11 == 0, i.e. no
// transition ever lands ON the breach state, even though a breach did occur
// (a lone breach at position 0, which never appears as a transition's "to"
// side). n10 == 1 is the only nonzero count contributing to the "from
// breach" row; n00 == 3 is the only nonzero count in the "from no-breach"
// row. Every term still short-circuits cleanly (see header comment).
TEST(VarValidation, ChristoffersenHandlesLoneLeadingBreachByLimit) {
  const std::array<bool, 5> sequence{true, false, false, false, false};
  const auto result = christoffersen(std::span<const bool>(sequence), 0.99);
  ASSERT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  EXPECT_DOUBLE_EQ(result->lr_independence, 0.0);
  EXPECT_DOUBLE_EQ(result->p_independence, 1.0);
}

TEST(VarValidation, ChristoffersenRejectsSequencesShorterThanTwoObservations) {
  const std::array<bool, 1> one{true};
  EXPECT_FALSE(christoffersen(std::span<const bool>(one), 0.99));
  EXPECT_FALSE(christoffersen(std::span<const bool>{}, 0.99));
}

TEST(VarValidation, ChristoffersenPropagatesInvalidConfidenceFromKupiec) {
  const std::array<bool, 4> sequence{false, true, false, true};
  EXPECT_FALSE(christoffersen(std::span<const bool>(sequence), 0.0));
  EXPECT_FALSE(christoffersen(std::span<const bool>(sequence), 1.0));
}

} // namespace
