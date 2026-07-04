// optimizer_recenter_test.cpp — S4-2 [CORRECTNESS, B2]: cap-clip-renorm must not
// break dollar-neutrality. PortfolioOptimizer::project demeans, gross-normalizes,
// then cap-clip-renorms; cap_clip_renorm's final pass scales ONLY the sub-cap
// (unpinned) names to hit the gross budget, holding pinned names exactly at
// +/-cap. When the cap binds an unequal mass of longs vs shorts, the pinned
// mass alone is generally != 0, so the book exits with Sigma w != 0 even though
// it entered perfectly centered -- a spurious net exposure on a book the caller
// declared dollar-neutral (a fake market-beta return).
//
// By-construction fixture (found by a small numeric search over asymmetric-clip
// vectors, then hand-verified against the exact `project`/`cap_clip_renorm`
// algorithm in a Python transliteration): 4-name book, cap=0.30, L=1.0,
// dollar_neutral=true, raw alpha targets (fed through solve()'s lambda=0 path,
// which reduces project()'s input to demean(alpha) before gross-normalize) such
// that after demean+gross_normalize the vector is EXACTLY
//   v = [0.40, 0.10, -0.35, -0.15]   (sum == 0, sum|v| == 1.0 == L already)
// Post cap-clip (0.40 and -0.35 pin to +/-0.30; 0.10 and -0.15 stay unbound):
// the CURRENT (buggy) code leaves net = -0.08 (hand-verified: pinned sum
// 0.30-0.30=0, unbound sum 0.10*scale + -0.15*scale != 0 after the deficit-only
// renorm). The FIX (alternating demean_live + cap_clip_renorm, kRecenterIters
// passes) drives net to float noise (<1e-9) while max|w|<=cap and Sigma|w|==L
// hold throughout.

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/optimizer.hpp"

namespace atxtest_optimizer_recenter_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::FactorModel;
using atx::engine::risk::OptimizerConfig;
using atx::engine::risk::PortfolioOptimizer;

// A dead (zero-factor) FactorModel: X=0 (no factor loadings), F=[[0.01]],
// D=ones (unit specific variance) -- V == diag(D), purely specific. With
// risk_aversion==0 (these tests' setting, the lambda=0 "pure-alpha
// WeightPolicy" path, see the header) V is never inverted at all; the model is
// a placeholder value the solver signature requires. Mirrors
// risk_optimizer_test.cpp's benign_model/make_model convention.
[[nodiscard]] FactorModel dead_model(usize m) {
  MatX x = MatX::Zero(static_cast<Eigen::Index>(m), 1);
  MatX f(1, 1);
  f << 0.01;
  VecX d = VecX::Constant(static_cast<Eigen::Index>(m), 1.0);
  auto r = FactorModel::create(x, f, d, /*fit_begin=*/0U, /*fit_end=*/10U);
  EXPECT_TRUE(r.has_value()) << "dead_model must build";
  return std::move(*r);
}

// ===========================================================================
//  optimizer_neutral_after_clip: the asymmetric-clip fixture must return
//  Sigma w ~ 0 after solve() (lambda=0, dollar_neutral, a binding cap), while
//  max|w|<=cap and Sigma|w|==L hold. RED (pre-fix): net ~ -0.08 (fails at 1e-9).
// ===========================================================================
TEST(OptimizerRecenter, NeutralAfterClip) {
  // Raw alpha chosen so demean(alpha) (lambda=0's smooth target, before
  // gross-normalize) is PROPORTIONAL to [0.40, 0.10, -0.35, -0.15] (already
  // zero-mean: 0.40+0.10-0.35-0.15=0.0) -- gross_normalize then scales it to
  // Sigma|v|==L==1.0, which this vector ALREADY satisfies
  // (0.40+0.10+0.35+0.15==1.0), so v is unchanged by gross_normalize too.
  const std::vector<f64> alpha{0.40, 0.10, -0.35, -0.15};
  const FactorModel V = dead_model(4);

  OptimizerConfig cfg{};
  cfg.risk_aversion = 0.0; // lambda=0: smooth target == demean(alpha) exactly
  cfg.turnover_penalty = 0.0;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 0.30; // binds 0.40 and -0.35; leaves 0.10 and -0.15 unbound
  cfg.dollar_neutral = true;
  cfg.max_iters = 64;

  const PortfolioOptimizer opt{cfg};
  auto res = opt.solve(std::span<const f64>{alpha}, V, std::span<const f64>{});
  ASSERT_TRUE(res.has_value()) << "solve must succeed on a well-formed dead model";
  const std::vector<f64> &w = res.value();
  ASSERT_EQ(w.size(), 4U);

  f64 sum = 0.0, gross = 0.0, max_abs = 0.0;
  for (const f64 x : w) {
    sum += x;
    gross += std::fabs(x);
    max_abs = std::max(max_abs, std::fabs(x));
  }
  EXPECT_NEAR(sum, 0.0, 1e-9)
      << "dollar-neutral book must have Sigma w ~ 0 after the cap clip+re-center; got " << sum;
  EXPECT_LE(max_abs, 0.30 + 1e-9) << "the per-name cap must still hold";
  EXPECT_NEAR(gross, 1.0, 1e-6) << "the gross leverage budget must still hold";
}

// ===========================================================================
//  clip_renorm_non_neutral_unchanged: with dollar_neutral=false the re-center is
//  a documented no-op -- the returned book must be BYTE-IDENTICAL to a solve
//  with the exact same inputs run through the SAME (pre- and post-fix
//  identical) code path, i.e. any book is accepted as-is once demean_live no-ops.
//  We assert the structural invariant this guarantees: the returned book need
//  NOT be net-neutral (unlike the dollar_neutral=true case above).
// ===========================================================================
TEST(OptimizerRecenter, ClipRenormNonNeutralUnchanged) {
  const std::vector<f64> alpha{0.40, 0.10, -0.35, -0.15};
  const FactorModel V = dead_model(4);

  OptimizerConfig cfg{};
  cfg.risk_aversion = 0.0;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 0.30;
  cfg.dollar_neutral = false; // neutrality NOT requested -- recenter must no-op
  cfg.max_iters = 64;

  const PortfolioOptimizer opt{cfg};
  auto res = opt.solve(std::span<const f64>{alpha}, V, std::span<const f64>{});
  ASSERT_TRUE(res.has_value());
  const std::vector<f64> &w = res.value();
  f64 gross = 0.0, max_abs = 0.0;
  for (const f64 x : w) {
    gross += std::fabs(x);
    max_abs = std::max(max_abs, std::fabs(x));
  }
  // The cap and gross budget still hold (invariants of cap_clip_renorm itself,
  // untouched by S4-2), but neutrality is NOT enforced -- this book need not
  // sum to 0 (the S4-2 recenter never runs on this branch).
  EXPECT_LE(max_abs, 0.30 + 1e-9) << "the per-name cap must still hold";
  EXPECT_NEAR(gross, 1.0, 1e-6) << "the gross leverage budget must still hold";
  // Twice-run byte-identity: the non-neutral path is a pure/deterministic
  // function of (alpha, V, cfg) whether or not S4-2's recenter code exists on
  // the neutral branch (it is gated OFF here) -- re-running must reproduce the
  // exact same book.
  auto res2 = opt.solve(std::span<const f64>{alpha}, V, std::span<const f64>{});
  ASSERT_TRUE(res2.has_value());
  ASSERT_EQ(res2.value().size(), w.size());
  for (usize i = 0; i < w.size(); ++i) {
    EXPECT_EQ(w[i], res2.value()[i]) << "non-neutral path must be bit-reproducible at i=" << i;
  }
}

} // namespace atxtest_optimizer_recenter_test
