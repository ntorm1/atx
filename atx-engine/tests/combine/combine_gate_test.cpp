// combine_gate_test.cpp — P4-3: AlphaGate (orthogonality + quality gates) and the
// shared pairwise-complete Pearson helper.
//
// AlphaGate::admit screens a candidate alpha against the GateConfig floors
// (fitness / Sharpe / turnover) AND the diversification gate (max |pairwise-
// complete Pearson| of the candidate's PnL vs each accepted pool member must be
// <= max_pool_corr). The verdict is the FIRST failing condition in the fixed
// order (fitness → Sharpe → turnover → correlation) so it is deterministic. The
// correlation cost is paid LAZILY — only after the three floors pass — but the
// resulting verdict is identical to the eager order.
//
// The check order aligns admission with the selection objective: fitness (WQ-
// aligned) is the dominant primary gate; Sharpe is a low sanity floor (0.25)
// with DSR (computed factory-side) as the real significance gate.
//
// pairwise_complete_corr (combine/correlation.hpp) is the one shared NaN policy
// helper the combiner (P4-4) will reuse: Pearson over indices where BOTH legs are
// non-NaN; < 2 valid pairs or zero variance in either leg → 0.0 (a degenerate /
// no-overlap pair contributes 0 correlation, so it can never trip the magnitude
// gate falsely).
//
// Coverage (plan §8 P4-3):
//   * below-fitness      -> RejectFitness   (primary gate, WQ-aligned)
//   * below-Sharpe       -> RejectSharpe    (sanity floor; below 0.25)
//   * above-turnover     -> RejectTurnover
//   * perfect copy (corr +1.0)   -> RejectCorrelated
//   * anti-correlated (corr -1.0, |corr|=1) -> RejectCorrelated (magnitude gate)
//   * first alpha (empty pool)   -> Accept (corr_to_pool = 0)
//   * orthogonal (corr ~0) clearing floors -> Accept
//   * fixed order: fail BOTH sharpe and turnover -> RejectSharpe (sharpe before turnover)
//   * boundary: pairwise-complete with interleaved NaN gaps (hand value)
//   * all-NaN candidate -> corr 0 -> Accept when floors are cleared
//   * direct pairwise_complete_corr unit tests (hand Pearson + NaN-gap case)

#include <cmath>  // std::isnan, std::sqrt
#include <limits> // std::numeric_limits (NaN fixture)
#include <span>   // std::span
#include <vector> // std::vector (fixture storage)

#include <gtest/gtest.h>

#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/combine/correlation.hpp" // pairwise_complete_corr
#include "atx/engine/combine/gate.hpp"        // GateConfig, GateVerdict, AlphaGate
#include "atx/engine/combine/metrics.hpp"     // AlphaMetrics
#include "atx/engine/combine/store.hpp"       // AlphaStore

namespace atxtest_combine_gate_test {

using atx::f64;
using atx::usize;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::AlphaStore;
using atx::engine::combine::GateConfig;
using atx::engine::combine::GateVerdict;
using atx::engine::combine::pairwise_complete_corr;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// Metrics that clear ALL default floors (min_sharpe=0.25, min_fitness=1.0,
// max_turnover=0.70) so a test can isolate the correlation gate. Only the
// floored fields matter; the rest are filler.
[[nodiscard]] AlphaMetrics passing_metrics() {
  return AlphaMetrics{/*sharpe*/ 2.0,      /*turnover*/ 0.30, /*returns*/ 0.10,
                      /*drawdown*/ 0.1,    /*margin*/ 1.0,    /*fitness*/ 2.0,
                      /*holding_days*/ 3.3};
}

// A constant (flat) position cross-section for the store fixture — the gate
// reads only the PnL row, so positions are inert filler of the right length.
[[nodiscard]] std::vector<f64> flat_positions(usize periods, usize insts) {
  return std::vector<f64>(periods * insts, 0.0);
}

// Insert one PnL row into the pool with dummy metrics + nullptr source. The gate
// reads only the member PnL stream, so metrics/source are inert here.
void insert_member(AlphaStore &pool, std::span<const f64> pnl, usize insts) {
  const auto pos = flat_positions(pnl.size(), insts);
  const auto r = pool.insert(/*source*/ nullptr, pnl, pos, AlphaMetrics{});
  ASSERT_TRUE(r.has_value());
}

// ===========================================================================
//  pairwise_complete_corr — direct unit tests
// ===========================================================================

TEST(Correlation, HandComputedPearson) {
  // a = {1,2,3,4}, b = {2,4,6,8} == 2a -> perfect positive correlation +1.
  const std::vector<f64> a{1.0, 2.0, 3.0, 4.0};
  const std::vector<f64> b{2.0, 4.0, 6.0, 8.0};
  EXPECT_NEAR(pairwise_complete_corr(a, b), 1.0, 1e-12);
}

TEST(Correlation, PerfectNegative) {
  const std::vector<f64> a{1.0, 2.0, 3.0, 4.0};
  const std::vector<f64> b{4.0, 3.0, 2.0, 1.0};
  EXPECT_NEAR(pairwise_complete_corr(a, b), -1.0, 1e-12);
}

TEST(Correlation, NaNGapsPairwiseComplete) {
  // Only indices 1, 3, 4 are valid in BOTH legs. Over those pairs:
  //   a' = {2, 4, 5}, b' = {4, 8, 10} == 2a' -> Pearson +1.
  // The NaN-poisoned indices 0 and 2 are skipped, so the result is the corr of
  // the overlap ONLY.
  const std::vector<f64> a{kNaN, 2.0, 3.0, 4.0, 5.0};
  const std::vector<f64> b{1.0, 4.0, kNaN, 8.0, 10.0};
  EXPECT_NEAR(pairwise_complete_corr(a, b), 1.0, 1e-12);
}

TEST(Correlation, NaNGapHandValueNonTrivial) {
  // Overlap (both non-NaN) is indices 0,2,3: a'={1,3,4}, b'={2,5,6}.
  // mean_a=8/3, mean_b=13/3. Hand Pearson over these 3 pairs:
  //   Sxy = Σ(a-ma)(b-mb), Sxx = Σ(a-ma)^2, Syy = Σ(b-mb)^2.
  const std::vector<f64> a{1.0, kNaN, 3.0, 4.0};
  const std::vector<f64> b{2.0, 9.0, 5.0, 6.0};
  const f64 ma = (1.0 + 3.0 + 4.0) / 3.0;
  const f64 mb = (2.0 + 5.0 + 6.0) / 3.0;
  f64 sxy = 0.0;
  f64 sxx = 0.0;
  f64 syy = 0.0;
  const f64 av[3] = {1.0, 3.0, 4.0};
  const f64 bv[3] = {2.0, 5.0, 6.0};
  for (int i = 0; i < 3; ++i) {
    sxy += (av[i] - ma) * (bv[i] - mb);
    sxx += (av[i] - ma) * (av[i] - ma);
    syy += (bv[i] - mb) * (bv[i] - mb);
  }
  const f64 expected = sxy / std::sqrt(sxx * syy);
  EXPECT_NEAR(pairwise_complete_corr(a, b), expected, 1e-12);
}

TEST(Correlation, FewerThanTwoValidPairsReturnsZero) {
  // Only one overlapping valid pair (index 1) -> < 2 pairs -> 0.0 by convention.
  const std::vector<f64> a{kNaN, 5.0, kNaN};
  const std::vector<f64> b{1.0, 7.0, 3.0};
  EXPECT_EQ(pairwise_complete_corr(a, b), 0.0);
}

TEST(Correlation, ZeroVarianceLegReturnsZero) {
  // b is constant -> zero variance -> degenerate -> 0.0 (never trips the gate).
  const std::vector<f64> a{1.0, 2.0, 3.0, 4.0};
  const std::vector<f64> b{5.0, 5.0, 5.0, 5.0};
  EXPECT_EQ(pairwise_complete_corr(a, b), 0.0);
}

TEST(Correlation, AllNaNReturnsZero) {
  const std::vector<f64> a{kNaN, kNaN, kNaN};
  const std::vector<f64> b{1.0, 2.0, 3.0};
  EXPECT_EQ(pairwise_complete_corr(a, b), 0.0);
}

TEST(Correlation, MismatchedLengthAbortsInDebug) {
  // The equal-length precondition fires ATX_ASSERT (-> std::abort) in a debug
  // build; EXPECT_DEATH captures it. Documents the contract: the release build
  // is OOB-safe via the min() bound, but a mismatched call is still misuse and
  // must fail loudly in debug. The regex matches any output.
  const std::vector<f64> a{1.0, 2.0, 3.0, 4.0};
  const std::vector<f64> b{1.0, 2.0}; // shorter -> contract violation
  EXPECT_DEATH({ (void)pairwise_complete_corr(a, b); }, ".*");
}

// ===========================================================================
//  AlphaGate::admit — floor gates
// ===========================================================================

TEST(AlphaGate, BelowSharpeRejectsSharpe) {
  const AlphaGate gate{GateConfig{}};
  AlphaMetrics m = passing_metrics();
  m.sharpe = 0.1; // below min_sharpe (0.25); fitness=2.0 passes (primary gate)
  const AlphaStore pool;
  const std::vector<f64> cand{0.0, 0.01, 0.02};
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::RejectSharpe);
}

TEST(AlphaGate, BelowFitnessRejectsFitness) {
  const AlphaGate gate{GateConfig{}};
  AlphaMetrics m = passing_metrics();
  m.fitness = 0.5; // below min_fitness (1.0); fitness is the primary gate (first check)
  const AlphaStore pool;
  const std::vector<f64> cand{0.0, 0.01, 0.02};
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::RejectFitness);
}

TEST(AlphaGate, AboveTurnoverRejectsTurnover) {
  const AlphaGate gate{GateConfig{}};
  AlphaMetrics m = passing_metrics();
  m.turnover = 0.95; // above max_turnover (0.70), sharpe + fitness passing
  const AlphaStore pool;
  const std::vector<f64> cand{0.0, 0.01, 0.02};
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::RejectTurnover);
}

// ===========================================================================
//  Fixed-order determinism
// ===========================================================================

TEST(AlphaGate, FailsSharpeAndTurnoverReturnsSharpeFirst) {
  // A candidate that fails BOTH the Sharpe and turnover floors must report
  // RejectSharpe — in the fixed order (fitness→Sharpe→turnover), fitness
  // passes (2.0), then Sharpe=0.1 < min_sharpe=0.25 fires before turnover.
  const AlphaGate gate{GateConfig{}};
  AlphaMetrics m = passing_metrics();
  m.sharpe = 0.1;    // fail (below 0.25 sanity floor); fitness=2.0 passes first
  m.turnover = 0.99; // also fail, but Sharpe is checked before turnover
  const AlphaStore pool;
  const std::vector<f64> cand{0.0, 0.01, 0.02};
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::RejectSharpe);
}

// ===========================================================================
//  Correlation / diversification gate
// ===========================================================================

TEST(AlphaGate, EmptyPoolAcceptsFirstAlpha) {
  // Empty pool -> corr_to_pool = 0 -> the first floor-clearing alpha is admitted.
  const AlphaGate gate{GateConfig{}};
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand{0.0, 0.01, -0.02, 0.03};
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::Accept);
}

TEST(AlphaGate, PerfectCopyRejectsCorrelated) {
  // Candidate is an exact copy of the single pool member -> corr +1 > 0.7.
  const AlphaGate gate{GateConfig{}};
  const AlphaMetrics m = passing_metrics();
  AlphaStore pool;
  const std::vector<f64> member{0.0, 0.01, -0.02, 0.03, 0.04};
  insert_member(pool, member, /*insts*/ 1);
  const std::vector<f64> cand = member; // perfect copy
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::RejectCorrelated);
}

TEST(AlphaGate, AntiCorrelatedRejectsCorrelatedByMagnitude) {
  // Candidate is the negation of the member -> corr -1, |corr| = 1 > 0.7.
  // The magnitude gate (|corr|) must reject it.
  const AlphaGate gate{GateConfig{}};
  const AlphaMetrics m = passing_metrics();
  AlphaStore pool;
  const std::vector<f64> member{0.0, 0.01, -0.02, 0.03, 0.04};
  insert_member(pool, member, /*insts*/ 1);
  std::vector<f64> cand(member.size());
  for (usize i = 0; i < member.size(); ++i) {
    cand[i] = -member[i];
  }
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::RejectCorrelated);
}

TEST(AlphaGate, OrthogonalCandidateAccepts) {
  // Two members the candidate is ~uncorrelated with -> corr_to_pool ~ 0 <= 0.7.
  // Member A: {0,1,-1,0}; candidate orthogonal: {0,1,1,-1} chosen so the
  // mean-centered dot products are small relative to the norms.
  const AlphaGate gate{GateConfig{}};
  const AlphaMetrics m = passing_metrics();
  AlphaStore pool;
  const std::vector<f64> member_a{0.0, 1.0, -1.0, 0.0};
  insert_member(pool, member_a, /*insts*/ 1);
  // Candidate orthogonal to member_a: <a-mean, c-mean> = 0.
  const std::vector<f64> cand{0.0, -1.0, -1.0, 2.0};
  // Sanity: the helper agrees this is ~0 correlation.
  ASSERT_NEAR(pairwise_complete_corr(member_a, cand), 0.0, 1e-9);
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::Accept);
}

TEST(AlphaGate, MaxOverPoolUsesStrictestMember) {
  // Pool has an orthogonal member AND a perfectly correlated member. The gate
  // takes the MAX |corr| over members, so the perfect copy dominates -> reject.
  const AlphaGate gate{GateConfig{}};
  const AlphaMetrics m = passing_metrics();
  AlphaStore pool;
  const std::vector<f64> orthog{0.0, 1.0, -1.0, 0.0};
  const std::vector<f64> twin{0.0, 0.01, -0.02, 0.03};
  insert_member(pool, orthog, /*insts*/ 1);
  insert_member(pool, twin, /*insts*/ 1);
  const std::vector<f64> cand = twin; // perfectly correlated with the 2nd member
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::RejectCorrelated);
}

TEST(AlphaGate, NaNGapPoolCorrComputedOverOverlapOnly) {
  // Member and candidate have interleaved NaN gaps; the corr is computed only
  // over the overlapping valid dates. The overlap is a perfect copy -> reject.
  const AlphaGate gate{GateConfig{}};
  const AlphaMetrics m = passing_metrics();
  AlphaStore pool;
  const std::vector<f64> member{kNaN, 1.0, 2.0, kNaN, 4.0};
  insert_member(pool, member, /*insts*/ 1);
  // Candidate matches the member exactly on the overlap (indices 1,2,4) and is
  // NaN where the member is also NaN -> overlap corr = +1 -> reject.
  const std::vector<f64> cand{0.0, 1.0, 2.0, kNaN, 4.0};
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::RejectCorrelated);
}

TEST(AlphaGate, AllNaNCandidateAcceptsViaZeroCorr) {
  // An all-NaN candidate has 0 valid pairs vs. every member -> corr_to_pool = 0
  // (documented degenerate convention) -> it PASSES the correlation gate. The
  // floors are cleared here (passing_metrics), so the documented outcome is
  // Accept: the floors are responsible for catching a junk candidate, not the
  // corr gate.
  const AlphaGate gate{GateConfig{}};
  const AlphaMetrics m = passing_metrics();
  AlphaStore pool;
  const std::vector<f64> member{0.0, 0.01, -0.02, 0.03, 0.04};
  insert_member(pool, member, /*insts*/ 1);
  const std::vector<f64> cand{kNaN, kNaN, kNaN, kNaN, kNaN};
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::Accept);
}

TEST(AlphaGate, BoundaryAtMaxPoolCorrAccepts) {
  // corr exactly at max_pool_corr passes (<=). Build a member/candidate pair
  // whose pairwise corr equals the configured threshold within tolerance by
  // setting max_pool_corr to the measured corr.
  AlphaStore pool;
  const std::vector<f64> member{0.0, 1.0, 2.0, 3.0, 4.0};
  insert_member(pool, member, /*insts*/ 1);
  const std::vector<f64> cand{0.0, 1.0, 2.0, 3.0, 5.0}; // correlated but not identical
  const f64 c = pairwise_complete_corr(member, cand);
  GateConfig cfg;
  cfg.max_pool_corr = c; // threshold == measured corr -> boundary <= passes
  const AlphaGate gate{cfg};
  const AlphaMetrics m = passing_metrics();
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::Accept);
}


}  // namespace atxtest_combine_gate_test
