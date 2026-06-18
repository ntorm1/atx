// atx::engine::combine — conviction score tests (S10-1).
//
// Proves the continuous [0,1] confidence blend end-to-end: the three
// monotonicities (DSR up ⇒ up; PBO up ⇒ down; stability up ⇒ up), the exact
// explainability multipliers (HeadScratcher halves, PartlyExplained 0.75),
// [0,1] bounds + term clamping, determinism, the all-perfect alpha, and the
// fail-closed NaN abort (death test). Caught by `ctest -R Conviction`.

#include <gtest/gtest.h>

#include <limits> // std::numeric_limits (NaN literal for the death test)

#include "atx/engine/combine/conviction.hpp"
#include "atx/engine/eval/deflated_sharpe.hpp" // eval::DsrResult literals
#include "atx/engine/eval/pbo.hpp"             // eval::PboResult literals

namespace atxtest_combine_conviction_test {

using atx::engine::combine::conviction;
using atx::engine::combine::ConvictionConfig;
using atx::engine::combine::ConvictionScore;
using atx::engine::combine::ExplainFlag;
using atx::engine::eval::DsrResult;
using atx::engine::eval::PboResult;

namespace {

// Aggregate-init helpers: DsrResult{psr, sr_star, dsr, haircut} — only `dsr` is
// read by conviction(); PboResult{pbo, split_logits, mean_logit} — only `pbo`.
[[nodiscard]] DsrResult dsr_with(double d) { return DsrResult{d, 0.0, d, 0.0}; }
[[nodiscard]] PboResult pbo_with(double p) { return PboResult{p, {}, 0.0}; }

} // namespace

// 1. Higher DSR (all else equal) ⇒ strictly higher score.
TEST(Conviction, MonotoneInDsr) {
  const PboResult pbo = pbo_with(0.2);
  const ConvictionScore lo = conviction(dsr_with(0.5), pbo, 0.8, ExplainFlag::Explained);
  const ConvictionScore hi = conviction(dsr_with(0.9), pbo, 0.8, ExplainFlag::Explained);
  EXPECT_GT(hi.score, lo.score);
}

// 2. Higher PBO (all else equal) ⇒ strictly LOWER score (term is 1 - pbo).
TEST(Conviction, MonotoneInPboDown) {
  const DsrResult dsr = dsr_with(0.7);
  const ConvictionScore lo_pbo = conviction(dsr, pbo_with(0.1), 0.8, ExplainFlag::Explained);
  const ConvictionScore hi_pbo = conviction(dsr, pbo_with(0.6), 0.8, ExplainFlag::Explained);
  EXPECT_LT(hi_pbo.score, lo_pbo.score);
}

// 3. Higher OOS/IS stability ratio (within [0,1]) ⇒ higher score.
TEST(Conviction, MonotoneInStability) {
  const DsrResult dsr = dsr_with(0.6);
  const PboResult pbo = pbo_with(0.3);
  const ConvictionScore lo = conviction(dsr, pbo, 0.2, ExplainFlag::Explained);
  const ConvictionScore hi = conviction(dsr, pbo, 0.9, ExplainFlag::Explained);
  EXPECT_GT(hi.score, lo.score);
}

// 4. HeadScratcher halves: default cfg ⇒ score == 0.5 * Explained score
//    (1.0 Explained mult, 0.5 default head-scratcher discount).
TEST(Conviction, HeadScratcherHalvesExplained) {
  const DsrResult dsr = dsr_with(0.8);
  const PboResult pbo = pbo_with(0.25);
  const double ratio = 0.7;
  const ConvictionScore expl = conviction(dsr, pbo, ratio, ExplainFlag::Explained);
  const ConvictionScore head = conviction(dsr, pbo, ratio, ExplainFlag::HeadScratcher);
  EXPECT_DOUBLE_EQ(head.score, 0.5 * expl.score);
  EXPECT_DOUBLE_EQ(expl.explain_mult, 1.0);
  EXPECT_DOUBLE_EQ(head.explain_mult, 0.5);
}

// 5. PartlyExplained multiplier: score == 0.75 * Explained score (default cfg).
TEST(Conviction, PartlyExplainedMultiplier) {
  const DsrResult dsr = dsr_with(0.65);
  const PboResult pbo = pbo_with(0.3);
  const double ratio = 0.6;
  const ConvictionScore expl = conviction(dsr, pbo, ratio, ExplainFlag::Explained);
  const ConvictionScore partly = conviction(dsr, pbo, ratio, ExplainFlag::PartlyExplained);
  EXPECT_DOUBLE_EQ(partly.score, 0.75 * expl.score);
  EXPECT_DOUBLE_EQ(partly.explain_mult, 0.75);
}

// 6. Bounds: score ∈ [0,1] across a small input grid; stability_term clamps
//    (ratio = 2.0 ⇒ term 1.0; ratio = -1 ⇒ term 0).
TEST(Conviction, BoundsAndStabilityClamp) {
  const double dsrs[] = {0.0, 0.5, 1.0};
  const double pbos[] = {0.0, 0.5, 1.0};
  const double ratios[] = {-1.0, 0.0, 0.5, 1.0, 2.0};
  const ExplainFlag flags[] = {ExplainFlag::Explained, ExplainFlag::PartlyExplained,
                               ExplainFlag::HeadScratcher};
  for (const double d : dsrs) {
    for (const double p : pbos) {
      for (const double r : ratios) {
        for (const ExplainFlag f : flags) {
          const ConvictionScore s = conviction(dsr_with(d), pbo_with(p), r, f);
          EXPECT_GE(s.score, 0.0);
          EXPECT_LE(s.score, 1.0);
        }
      }
    }
  }
  // Stability clamp: ratio above 1 saturates to 1.0; below 0 floors to 0.
  const ConvictionScore over = conviction(dsr_with(0.5), pbo_with(0.5), 2.0, ExplainFlag::Explained);
  const ConvictionScore under =
      conviction(dsr_with(0.5), pbo_with(0.5), -1.0, ExplainFlag::Explained);
  EXPECT_DOUBLE_EQ(over.stability_term, 1.0);
  EXPECT_DOUBLE_EQ(under.stability_term, 0.0);
}

// 7. Deterministic: same inputs ⇒ byte-identical ConvictionScore (all 5 fields).
TEST(Conviction, DeterministicTwoRunsEqual) {
  const DsrResult dsr = dsr_with(0.73);
  const PboResult pbo = pbo_with(0.42);
  const ConvictionScore a = conviction(dsr, pbo, 0.61, ExplainFlag::PartlyExplained);
  const ConvictionScore b = conviction(dsr, pbo, 0.61, ExplainFlag::PartlyExplained);
  EXPECT_EQ(a.score, b.score);
  EXPECT_EQ(a.dsr_term, b.dsr_term);
  EXPECT_EQ(a.pbo_term, b.pbo_term);
  EXPECT_EQ(a.stability_term, b.stability_term);
  EXPECT_EQ(a.explain_mult, b.explain_mult);
}

// 8. All-perfect alpha: dsr=1, pbo=0, ratio=1, Explained ⇒ score == 1.0.
TEST(Conviction, AllPerfectIsOne) {
  const ConvictionScore s = conviction(dsr_with(1.0), pbo_with(0.0), 1.0, ExplainFlag::Explained);
  EXPECT_DOUBLE_EQ(s.score, 1.0);
  EXPECT_DOUBLE_EQ(s.dsr_term, 1.0);
  EXPECT_DOUBLE_EQ(s.pbo_term, 1.0);
  EXPECT_DOUBLE_EQ(s.stability_term, 1.0);
}

// 9. Fail-closed: a NaN dsr.dsr aborts (the debug finite guard). Cast to void —
//    the result is [[nodiscard]] and the call aborts before returning.
TEST(ConvictionDeathTest, NanDsrAborts) {
  const DsrResult bad = dsr_with(std::numeric_limits<double>::quiet_NaN());
  const PboResult ok = pbo_with(0.2);
  EXPECT_DEATH((void)conviction(bad, ok, 1.0, ExplainFlag::Explained), ".*");
}

} // namespace atxtest_combine_conviction_test
