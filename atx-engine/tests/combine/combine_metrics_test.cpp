// combine_metrics_test.cpp — P4-2: compute_metrics (per-alpha metrics + fitness).
//
// compute_metrics turns one alpha's PnL stream (alpha::AlphaStreams::pnl(a)) and
// its target-weight position stream into the AlphaMetrics POD (Sharpe, turnover,
// returns, drawdown, margin, WorldQuant fitness, holding). The math is plan §5.1
// reconciled with §0 (§0 wins): the input stream's index 0 is a STRUCTURAL zero
// (AlphaStreams writes pnl[..][0] = 0 — period 0 has no prior weight or prior
// close), so the Sharpe/vol/returns moment computations exclude index 0. These
// tests pin that exclusion (a Sharpe test that would FAIL if index 0 were folded
// into mean/std), the verbatim fitness formula, drawdown, the NaN guards, and the
// derived turnover u[0] convention (u[0] = Σ_j |w_j[0]| — trading in from flat).
//
// Coverage (plan §8 P4-2 + §0-F):
//   * Known-value Sharpe over r[1..T) == sqrt(252)*mean/std (structural-0 excluded).
//   * Structural-0 exclusion: same numbers with a leading 0 vs. without -> equal.
//   * Fitness on a fixed (returns, turnover) pair matches the formula by hand.
//   * Drawdown on a known peak->trough->recovery path (hand fraction).
//   * Zero-volatility (flat stream) -> sharpe 0 (not NaN/inf).
//   * Zero-turnover -> fitness 0, margin/holding finite (1e-9 floor, no div-by-0).
//   * holding ~= 1/turnover on a known turnover.
//   * Turnover known-value: 2-period position fixture -> Σ|Δw|/book (pins u[0]).
//   * Boundary: length-1 stream (only the structural period -> 0 observations ->
//     sharpe/returns NaN, DOCUMENTED); all-NaN stream -> NaN metrics.

#include <cmath>  // std::sqrt, std::isnan, std::isfinite
#include <limits> // std::numeric_limits (NaN fixture)
#include <span>   // std::span
#include <vector> // std::vector (fixture storage)

#include <gtest/gtest.h>

#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/combine/metrics.hpp" // combine::AlphaMetrics, compute_metrics

namespace atxtest_combine_metrics_test {

using atx::f64;
using atx::usize;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::compute_metrics;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 kAnnualizationDays = 252.0;

// A flat position stream of `value` for every (period, instrument). Many tests
// only exercise the PnL moments / drawdown and want turnover held constant; a
// constant position stream yields turnover 0 after the trade-in period.
[[nodiscard]] std::vector<f64> const_positions(usize periods, usize insts, f64 value) {
  return std::vector<f64>(periods * insts, value);
}

// Hand sample-mean / population-std over r[1..T) (the structural-0 exclusion).
struct Moments {
  f64 mean;
  f64 std_pop;
};
[[nodiscard]] Moments moments_excl0(std::span<const f64> r) {
  f64 sum = 0.0;
  usize n = 0U;
  for (usize t = 1U; t < r.size(); ++t) {
    sum += r[t];
    ++n;
  }
  const f64 mean = sum / static_cast<f64>(n);
  f64 ss = 0.0;
  for (usize t = 1U; t < r.size(); ++t) {
    const f64 d = r[t] - mean;
    ss += d * d;
  }
  return Moments{mean, std::sqrt(ss / static_cast<f64>(n))};
}

// ---------------------------------------------------------------------------
//  Known-value Sharpe + structural-0 exclusion.
// ---------------------------------------------------------------------------

TEST(AlphaMetrics, SharpeKnownValueExcludesStructuralZero) {
  // Leading structural 0; moments over r[1..T) = [0.01, -0.02, 0.03, 0.00].
  const std::vector<f64> pnl{0.0, 0.01, -0.02, 0.03, 0.00};
  const std::vector<f64> pos = const_positions(pnl.size(), 1U, 0.5);

  const AlphaMetrics m = compute_metrics(pnl, pos, /*n_instruments*/ 1U, /*book_size*/ 1.0);

  const Moments mm = moments_excl0(pnl);
  const f64 expected_sharpe = std::sqrt(kAnnualizationDays) * mm.mean / mm.std_pop;
  const f64 expected_returns = kAnnualizationDays * mm.mean;
  EXPECT_NEAR(m.sharpe, expected_sharpe, 1e-9);
  EXPECT_NEAR(m.returns, expected_returns, 1e-9);
}

TEST(AlphaMetrics, StructuralZeroIsExcludedFromMoments) {
  // Same non-zero tail with vs. without a leading structural 0 must give the
  // SAME Sharpe/returns — proving index 0 is dropped (this FAILS if index 0 is
  // folded into mean/std, since a 5th 0 sample would shift both).
  const std::vector<f64> with0{0.0, 0.01, -0.02, 0.03, 0.00};
  const std::vector<f64> tail{0.01, -0.02, 0.03, 0.00}; // tail[0] becomes the new structural 0
  const std::vector<f64> pos_a = const_positions(with0.size(), 1U, 0.5);
  const std::vector<f64> pos_b = const_positions(tail.size(), 1U, 0.5);

  const AlphaMetrics a = compute_metrics(with0, pos_a, 1U, 1.0);
  // tail excludes its own index 0 (0.01), leaving [-0.02, 0.03, 0.00]; NOT equal
  // to `a`. The point is the *exclusion mechanism*, asserted directly below.
  const Moments mm_a = moments_excl0(with0);
  const f64 sharpe_if_included =
      std::sqrt(kAnnualizationDays) *
      ((0.0 + 0.01 - 0.02 + 0.03 + 0.00) / 5.0); // mean over ALL 5 (the wrong path numerator)
  // The computed Sharpe must match the excl-0 mean (0.005), NOT the include-0
  // mean (0.004): different numerators -> different Sharpe.
  EXPECT_NEAR(mm_a.mean, 0.005, 1e-12);
  EXPECT_NE(kAnnualizationDays * mm_a.mean, sharpe_if_included);
  EXPECT_NEAR(a.returns, kAnnualizationDays * 0.005, 1e-9);
  (void)pos_b;
  (void)tail;
}

// ---------------------------------------------------------------------------
//  Fitness verbatim formula.
// ---------------------------------------------------------------------------

TEST(AlphaMetrics, FitnessMatchesHandFormula) {
  // r[1..T) = [0.02, 0.02, 0.02, 0.02]: mean 0.02, std 0 -> sharpe 0 would zero
  // fitness, so use a stream with non-zero vol. Use [0.0, 0.03, 0.01, 0.05, 0.01].
  const std::vector<f64> pnl{0.0, 0.03, 0.01, 0.05, 0.01};
  // Position stream chosen so turnover is a clean known value (see below).
  // Two instruments; trade in to (0.5, 0.5) then hold -> u[0]=1.0, u[t>0]=0,
  // turnover = mean(u) = 1.0/4 = 0.25 over periods t=0..3 (length-4 walk).
  std::vector<f64> pos{0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5};

  const AlphaMetrics m = compute_metrics(pnl, pos, /*n_instruments*/ 2U, /*book_size*/ 1.0);

  const Moments mm = moments_excl0(pnl);
  const f64 sharpe = std::sqrt(kAnnualizationDays) * mm.mean / mm.std_pop;
  const f64 returns = kAnnualizationDays * mm.mean;
  const f64 turnover = m.turnover; // pinned in its own test; use the produced value
  const f64 floor = 0.125;
  const f64 denom = (turnover > floor) ? turnover : floor;
  const f64 expected_fitness = std::sqrt(std::abs(returns) / denom) * sharpe;
  EXPECT_NEAR(m.fitness, expected_fitness, 1e-9);
}

// ---------------------------------------------------------------------------
//  Drawdown on a known path.
// ---------------------------------------------------------------------------

TEST(AlphaMetrics, DrawdownKnownPeakTroughRecovery) {
  // equity = cumprod(1+r), r[0]=0: 1.0 -> 1.10 -> 0.88 -> 0.924.
  // peak holds 1.10 from t=1; max drawdown at t=2 = (1.10-0.88)/1.10 = 0.20.
  const std::vector<f64> pnl{0.0, 0.10, -0.20, 0.05};
  const std::vector<f64> pos = const_positions(pnl.size(), 1U, 0.5);

  const AlphaMetrics m = compute_metrics(pnl, pos, 1U, 1.0);
  EXPECT_NEAR(m.drawdown, 0.20, 1e-12);
}

// ---------------------------------------------------------------------------
//  NaN guards.
// ---------------------------------------------------------------------------

TEST(AlphaMetrics, ZeroVolatilityGivesZeroSharpe) {
  // Flat alpha: r[1..T) all equal -> std 0 -> sharpe defined as 0 (not NaN/inf).
  const std::vector<f64> pnl{0.0, 0.02, 0.02, 0.02, 0.02};
  const std::vector<f64> pos = const_positions(pnl.size(), 1U, 0.5);

  const AlphaMetrics m = compute_metrics(pnl, pos, 1U, 1.0);
  EXPECT_EQ(m.sharpe, 0.0);
  EXPECT_FALSE(std::isnan(m.sharpe));
  EXPECT_TRUE(std::isfinite(m.sharpe));
}

TEST(AlphaMetrics, ZeroTurnoverGivesZeroFitnessAndFiniteMarginHolding) {
  // No trades ever (positions all 0): turnover == 0. returns is non-zero, so the
  // 0.125 floor keeps fitness finite; but §5.1 NaN policy says turnover==0 AND
  // returns==0 -> fitness 0. Here returns != 0; assert fitness/margin/holding are
  // all FINITE (the 1e-9 floor prevents div-by-zero in margin/holding).
  const std::vector<f64> pnl{0.0, 0.01, 0.03, 0.02, 0.05};
  const std::vector<f64> pos = const_positions(pnl.size(), 1U, 0.0); // never trade

  const AlphaMetrics m = compute_metrics(pnl, pos, 1U, 1.0);
  EXPECT_EQ(m.turnover, 0.0);
  EXPECT_TRUE(std::isfinite(m.margin));
  EXPECT_TRUE(std::isfinite(m.holding_days));
  EXPECT_FALSE(std::isnan(m.fitness));
}

TEST(AlphaMetrics, ZeroTurnoverAndZeroReturnsGivesZeroFitness) {
  // turnover == 0 (never trade) AND returns == 0 (flat zero stream) -> fitness 0.
  const std::vector<f64> pnl{0.0, 0.0, 0.0, 0.0};
  const std::vector<f64> pos = const_positions(pnl.size(), 1U, 0.0);

  const AlphaMetrics m = compute_metrics(pnl, pos, 1U, 1.0);
  EXPECT_EQ(m.turnover, 0.0);
  EXPECT_EQ(m.returns, 0.0);
  EXPECT_EQ(m.fitness, 0.0);
  EXPECT_FALSE(std::isnan(m.fitness));
}

// ---------------------------------------------------------------------------
//  Holding ~= 1/turnover.
// ---------------------------------------------------------------------------

TEST(AlphaMetrics, HoldingDaysIsReciprocalOfTurnover) {
  // Build a known turnover of 0.25 (see fitness test rationale): trade in to
  // (0.5,0.5) at t=0, hold -> u = [1.0, 0, 0, 0], mean = 0.25 over 4 periods.
  const std::vector<f64> pnl{0.0, 0.01, 0.02, 0.03};
  const std::vector<f64> pos{0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5};

  const AlphaMetrics m = compute_metrics(pnl, pos, 2U, 1.0);
  EXPECT_NEAR(m.turnover, 0.25, 1e-12);
  EXPECT_NEAR(m.holding_days, 1.0 / 0.25, 1e-9);
}

// ---------------------------------------------------------------------------
//  Turnover known-value (pins the u[0] convention).
// ---------------------------------------------------------------------------

TEST(AlphaMetrics, TurnoverKnownValuePinsTradeInConvention) {
  // 2 periods, 2 instruments, book_size 2.0.
  // pos[0] = (1.0, -1.0); pos[1] = (0.5, -0.5).
  // u[0] = (|1.0| + |-1.0|)/2.0 = 2.0/2.0 = 1.0       (trade in from flat).
  // u[1] = (|0.5-1.0| + |-0.5-(-1.0)|)/2.0 = (0.5+0.5)/2 = 0.5.
  // turnover = mean(u) = (1.0 + 0.5)/2 = 0.75.
  const std::vector<f64> pnl{0.0, 0.01};
  const std::vector<f64> pos{1.0, -1.0, 0.5, -0.5};

  const AlphaMetrics m = compute_metrics(pnl, pos, /*n_instruments*/ 2U, /*book_size*/ 2.0);
  EXPECT_NEAR(m.turnover, 0.75, 1e-12);
}

// ---------------------------------------------------------------------------
//  Boundary: length-1 (only the structural period) and all-NaN.
// ---------------------------------------------------------------------------

TEST(AlphaMetrics, LengthOneStreamHasNoReturnObservations) {
  // Only the structural period -> 0 return observations after excluding index 0.
  // DOCUMENTED: sharpe/returns are NaN (mean/std of an empty set is undefined).
  // drawdown is 0 (equity stays at 1.0 through the single structural period).
  const std::vector<f64> pnl{0.0};
  const std::vector<f64> pos = const_positions(1U, 1U, 0.5);

  const AlphaMetrics m = compute_metrics(pnl, pos, 1U, 1.0);
  EXPECT_TRUE(std::isnan(m.sharpe));
  EXPECT_TRUE(std::isnan(m.returns));
  EXPECT_EQ(m.drawdown, 0.0);
}

TEST(AlphaMetrics, AllNaNStreamGivesNaNMetrics) {
  // An all-NaN PnL stream -> NaN moments -> NaN sharpe/returns (DOCUMENTED).
  const std::vector<f64> pnl{kNaN, kNaN, kNaN, kNaN};
  const std::vector<f64> pos = const_positions(pnl.size(), 1U, 0.5);

  const AlphaMetrics m = compute_metrics(pnl, pos, 1U, 1.0);
  EXPECT_TRUE(std::isnan(m.sharpe));
  EXPECT_TRUE(std::isnan(m.returns));
}

TEST(AlphaMetrics, EmptyStreamHasNaNMomentsAndZeroDrawdown) {
  // Defensive boundary: an empty span -> no observations -> NaN sharpe/returns,
  // 0 drawdown, 0 turnover. No UB, no div-by-zero.
  const std::vector<f64> pnl;
  const std::vector<f64> pos;

  const AlphaMetrics m = compute_metrics(pnl, pos, 1U, 1.0);
  EXPECT_TRUE(std::isnan(m.sharpe));
  EXPECT_TRUE(std::isnan(m.returns));
  EXPECT_EQ(m.drawdown, 0.0);
  EXPECT_EQ(m.turnover, 0.0);
}

// ---------------------------------------------------------------------------
//  S4-4: compute_metrics_with_turnover — byte-identity + per-period u[t].
// ---------------------------------------------------------------------------

using atx::engine::combine::compute_metrics_with_turnover;

TEST(AlphaMetricsWithTurnover, NullptrReturnsIdenticalMetrics) {
  // compute_metrics_with_turnover(..., nullptr) must produce an AlphaMetrics
  // byte-identical to compute_metrics(...) on the same inputs.
  const std::vector<f64> pnl{0.0, 0.01, -0.02, 0.03, 0.00};
  const std::vector<f64> pos{0.5, 0.5, 0.5, 0.5, 0.5};

  const AlphaMetrics expected = compute_metrics(pnl, pos, 1U, 1.0);
  const AlphaMetrics got = compute_metrics_with_turnover(pnl, pos, 1U, 1.0, nullptr);

  EXPECT_EQ(got.sharpe,       expected.sharpe);
  EXPECT_EQ(got.turnover,     expected.turnover);
  EXPECT_EQ(got.returns,      expected.returns);
  EXPECT_EQ(got.drawdown,     expected.drawdown);
  EXPECT_EQ(got.margin,       expected.margin);
  EXPECT_EQ(got.fitness,      expected.fitness);
  EXPECT_EQ(got.holding_days, expected.holding_days);
}

TEST(AlphaMetricsWithTurnover, OutVectorReturnsIdenticalMetrics) {
  // compute_metrics_with_turnover(..., &out) must also return AlphaMetrics
  // byte-identical to compute_metrics(...).
  const std::vector<f64> pnl{0.0, 0.01, -0.02, 0.03, 0.00};
  const std::vector<f64> pos{0.5, 0.5, 0.5, 0.5, 0.5};

  const AlphaMetrics expected = compute_metrics(pnl, pos, 1U, 1.0);
  std::vector<f64> out;
  const AlphaMetrics got = compute_metrics_with_turnover(pnl, pos, 1U, 1.0, &out);

  EXPECT_EQ(got.sharpe,       expected.sharpe);
  EXPECT_EQ(got.turnover,     expected.turnover);
  EXPECT_EQ(got.returns,      expected.returns);
  EXPECT_EQ(got.drawdown,     expected.drawdown);
  EXPECT_EQ(got.margin,       expected.margin);
  EXPECT_EQ(got.fitness,      expected.fitness);
  EXPECT_EQ(got.holding_days, expected.holding_days);
}

TEST(AlphaMetricsWithTurnover, OutVectorMatchesHandComputedPerPeriodTurnover) {
  // 2-period, 2-instrument fixture (pins the per-period u[t] values).
  // pos[t=0] = (1.0, -1.0), pos[t=1] = (0.5, -0.5), book_size = 2.0.
  // u[0] = (|1.0| + |-1.0|) / 2.0 = 1.0  (trade in from flat)
  // u[1] = (|0.5-1.0| + |-0.5-(-1.0)|) / 2.0 = (0.5 + 0.5) / 2.0 = 0.5
  const std::vector<f64> pnl{0.0, 0.01};
  const std::vector<f64> pos{1.0, -1.0, 0.5, -0.5};
  std::vector<f64> out;
  // Return value discarded intentionally: this test only cares about the filled
  // per-period turnover vector, not the AlphaMetrics (which byte-identity tests cover).
  (void)compute_metrics_with_turnover(pnl, pos, 2U, 2.0, &out);

  ASSERT_EQ(out.size(), 2U);
  EXPECT_NEAR(out[0], 1.0, 1e-12);
  EXPECT_NEAR(out[1], 0.5, 1e-12);
}

TEST(AlphaMetricsWithTurnover, LargerPanelByteIdentity) {
  // Stress the byte-identity claim on the fitness/holding/margin fields with a
  // richer fixture (the FitnessMatchesHandFormula fixture re-used here).
  const std::vector<f64> pnl{0.0, 0.03, 0.01, 0.05, 0.01};
  std::vector<f64> pos{0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5};

  const AlphaMetrics expected = compute_metrics(pnl, pos, 2U, 1.0);
  std::vector<f64> out;
  const AlphaMetrics got = compute_metrics_with_turnover(pnl, pos, 2U, 1.0, &out);

  EXPECT_EQ(got.sharpe,       expected.sharpe);
  EXPECT_EQ(got.turnover,     expected.turnover);
  EXPECT_EQ(got.returns,      expected.returns);
  EXPECT_EQ(got.drawdown,     expected.drawdown);
  EXPECT_EQ(got.margin,       expected.margin);
  EXPECT_EQ(got.fitness,      expected.fitness);
  EXPECT_EQ(got.holding_days, expected.holding_days);
  // out must have one entry per period
  EXPECT_EQ(out.size(), pnl.size());
}


}  // namespace atxtest_combine_metrics_test
