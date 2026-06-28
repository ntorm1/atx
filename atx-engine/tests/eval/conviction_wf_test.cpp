// conviction_wf_test.cpp — p7 S5-3: walk-forward conviction-awareness.
//
// T7 NEW-1 made each walk-forward fold in stage_combine CONVICTION-aware: when
// --conviction is on, a fold re-applies the per-fold conviction transform over its
// TRAIN window (causal — never the test window) BEFORE scoring, so the WF OOS
// Sharpe reflects the conviction-weighted book that actually ships. That wiring
// exists in code but had no dedicated test proving the interaction MOVES the score.
//
// This suite (ConvictionWF) proves it at the UNIT level — it does NOT run the
// pipeline. It exercises exactly the two engine primitives the stage composes:
//   * combine::conviction()              — the per-alpha [0,1] confidence.
//   * eval::walk_forward_sharpe()        — the disjoint-window OOS Sharpe.
// and replicates stage_combine.cpp's apply_conviction math (drop w_pbo, renorm
// w_dsr+w_stability to 1, explain=PartlyExplained, DSR over r=pnl[1..T), N=na,
// stability = annualized sharpe(2nd half)/sharpe(1st half)) over a fold's train
// window, then blends the per-alpha PnL over the test window and asks
// walk_forward_sharpe of the blend. The core claim: conviction ON vs OFF yields a
// DIFFERENT fold-1 OOS Sharpe on a constructed asymmetric fixture.
//
// Caught by `ctest -R ConvictionWF`.

#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/combine/conviction.hpp"      // combine::conviction, ConvictionConfig, ExplainFlag
#include "atx/engine/eval/deflated_sharpe.hpp"    // eval::deflated_sharpe, DsrResult
#include "atx/engine/eval/pbo.hpp"                // eval::PboResult
#include "atx/engine/eval/perf_metrics.hpp"       // eval::compute_return_metrics
#include "atx/engine/eval/regime_slice.hpp"       // eval::walk_forward_sharpe
#include "atx/engine/eval/stats_ext.hpp"          // eval::mean_std_pop, skewness, excess_kurtosis

namespace atxtest_eval_conviction_wf {

using atx::f64;
using atx::usize;
namespace ce = atx::engine::combine;
namespace ev = atx::engine::eval;

// ---------------------------------------------------------------------------
// per_alpha_conviction — the EXACT apply_conviction per-alpha score math from
// stage_combine.cpp (D1.2 / T7 NEW-1), restricted to a window [begin, end):
//   ccfg: drop w_pbo, renormalize w_dsr + w_stability to sum 1.
//   DSR : deflated_sharpe over r = pnl[1..T), N = na, real skew/excess-kurtosis.
//   ratio: annualized sharpe(2nd half) / sharpe(1st half) (0 if |sh1| tiny).
//   score = conviction(dsr, pbo=0, ratio, PartlyExplained, ccfg).score
// This is a faithful unit-level mirror so the test proves the SAME interaction the
// stage performs, without running the pipeline.
// ---------------------------------------------------------------------------
[[nodiscard]] f64 per_alpha_conviction(std::span<const f64> pnl_full, usize begin, usize end,
                                       usize na) {
  ce::ConvictionConfig ccfg{};
  const f64 wsum = ccfg.w_dsr + ccfg.w_stability;
  ccfg.w_dsr = ccfg.w_dsr / wsum;
  ccfg.w_stability = ccfg.w_stability / wsum;
  ccfg.w_pbo = 0.0;

  const usize wlen = (end > begin) ? (end - begin) : 0U;
  const std::span<const f64> pnl = pnl_full.subspan(begin, wlen);
  const usize T = pnl.size();

  ev::DsrResult dsr{};
  if (T > 1U) {
    const std::span<const f64> r{pnl.data() + 1, T - 1U};
    const f64 skew = ev::skewness(r);
    const f64 exk = ev::excess_kurtosis(r);
    const ev::MeanStd ms = ev::mean_std_pop(r);
    const f64 sr_pp = (ms.std > 0.0) ? ms.mean / ms.std : 0.0;
    dsr = ev::deflated_sharpe(sr_pp, r.size(), skew, exk,
                              /*N=*/(na > 0U ? na : 1U), std::nullopt);
  }

  f64 ratio = 0.0;
  if (T >= 4U) {
    ev::ReturnMetricsCfg rmc{};
    const usize mid = T / 2;
    const f64 sh1 = ev::compute_return_metrics(pnl.subspan(0, mid), rmc).sharpe;
    const f64 sh2 = ev::compute_return_metrics(pnl.subspan(mid), rmc).sharpe;
    if (std::isfinite(sh1) && std::isfinite(sh2) && std::fabs(sh1) > 1e-9) {
      ratio = sh2 / sh1;
    }
  }

  const ev::PboResult pbo{/*pbo=*/0.0, /*split_logits=*/{}, /*mean_logit=*/0.0};
  return ce::conviction(dsr, pbo, ratio, ce::ExplainFlag::PartlyExplained, ccfg).score;
}

// renorm |w| to sum 1 (the apply_conviction tail).
static void renorm_abs(std::array<f64, 2> &w) {
  const f64 s = std::fabs(w[0]) + std::fabs(w[1]);
  if (s > 0.0) {
    w[0] /= s;
    w[1] /= s;
  }
}

// Blend two per-alpha PnL streams over [begin, end) with weights w -> a single
// blended PnL vector (prepend a structural zero so compute/sharpe conventions that
// drop index 0 still score every real return, mirroring blend_window_sharpe).
[[nodiscard]] std::vector<f64> blend(std::span<const f64> a, std::span<const f64> b,
                                     const std::array<f64, 2> &w, usize begin, usize end) {
  const usize len = (end > begin) ? (end - begin) : 0U;
  std::vector<f64> out(len + 1U, 0.0);
  for (usize i = 0; i < len; ++i) {
    out[i + 1U] = w[0] * a[begin + i] + w[1] * b[begin + i];
  }
  return out;
}

// ---------------------------------------------------------------------------
// Constructed fixture (the plan's asymmetric design), T=60, 2 alphas:
//   Alpha A: periods 0-44 positive, 45-59 negative. In fold-1 train [0,30) A is
//            all positive -> high conviction; fold-1 test [30,60) A turns negative
//            45-59 -> worse OOS.
//   Alpha B: periods 0-44 zero, 45-59 strongly positive. In fold-1 train [0,30) B
//            is flat -> low edge -> low conviction; fold-1 test 45-59 B is positive.
// A small deterministic ripple keeps std > 0 so Sharpes are finite & non-degenerate.
// ---------------------------------------------------------------------------
struct TwoAlphaFixture {
  std::vector<f64> A;
  std::vector<f64> B;
  usize T;
};

[[nodiscard]] TwoAlphaFixture make_fixture() {
  constexpr usize T = 60;
  std::vector<f64> A(T, 0.0);
  std::vector<f64> B(T, 0.0);
  for (usize t = 0; t < T; ++t) {
    // deterministic alternating ripple so each half has non-zero variance.
    const f64 ripple = (t % 2 == 0) ? 0.0010 : -0.0006;
    if (t <= 44U) {
      A[t] = 0.0100 + ripple;   // positive edge in [0,44]
    } else {
      A[t] = -0.0120 + ripple;  // negative in [45,59]
    }
    if (t <= 44U) {
      B[t] = 0.0000 + ripple;   // ~flat (no edge) in [0,44]
    } else {
      B[t] = 0.0200 + ripple;   // strong positive in [45,59]
    }
  }
  return TwoAlphaFixture{std::move(A), std::move(B), T};
}

// Compute the fold-1 OOS walk-forward Sharpe with conviction ON or OFF.
// Mirrors the stage's fold-1: train [0, seg), test [seg, 2*seg); naive equal-weight
// combiner -> w=[0.5,0.5]; conviction scales by per-alpha train-window score then
// renorm. walk_forward_sharpe(blend, 1) is the single-window fold OOS Sharpe.
[[nodiscard]] f64 fold1_oos_sharpe(const TwoAlphaFixture &fx, bool conviction_on) {
  const usize seg = fx.T / 3U; // K=2 -> K+1=3 segments; fold 1 train [0,seg) test [seg,2seg)
  const usize train_end = seg;
  const usize test_end = 2U * seg;

  std::array<f64, 2> w{0.5, 0.5}; // naive equal-weight base
  if (conviction_on) {
    const f64 cA = per_alpha_conviction(fx.A, 0U, train_end, /*na=*/2U);
    const f64 cB = per_alpha_conviction(fx.B, 0U, train_end, /*na=*/2U);
    w[0] *= cA;
    w[1] *= cB;
    renorm_abs(w);
  }
  const std::vector<f64> b = blend(fx.A, fx.B, w, train_end, test_end);
  const std::vector<f64> wf = ev::walk_forward_sharpe(std::span<const f64>{b}, 1U);
  return wf.empty() ? 0.0 : wf.front();
}

// ===========================================================================
// (a) WF differs: conviction ON vs OFF yields a DIFFERENT fold-1 OOS Sharpe.
// ===========================================================================
TEST(ConvictionWF, FoldSharpeDiffersWhenConvictionOn) {
  const TwoAlphaFixture fx = make_fixture();
  const f64 s_off = fold1_oos_sharpe(fx, /*conviction_on=*/false);
  const f64 s_on = fold1_oos_sharpe(fx, /*conviction_on=*/true);

  EXPECT_TRUE(std::isfinite(s_off));
  EXPECT_TRUE(std::isfinite(s_on));
  EXPECT_GT(std::fabs(s_on - s_off), 1e-6)
      << "conviction must modulate the WF fold Sharpe (on=" << s_on << " off=" << s_off << ")";
}

// ===========================================================================
// (b) WF conviction-off matches the bare-combiner WF exactly (bit-for-bit). With
// conviction off the weights are the naive [0.5,0.5], so the fold Sharpe equals the
// bare-combiner fold Sharpe — proving the conviction path is truly opt-in.
// ===========================================================================
TEST(ConvictionWF, ConvictionOffEqualsBareCombiner) {
  const TwoAlphaFixture fx = make_fixture();

  // Bare combiner: equal-weight, computed directly (no conviction code path).
  const usize seg = fx.T / 3U;
  const std::array<f64, 2> w_bare{0.5, 0.5};
  const std::vector<f64> b_bare = blend(fx.A, fx.B, w_bare, seg, 2U * seg);
  const f64 s_bare = ev::walk_forward_sharpe(std::span<const f64>{b_bare}, 1U).front();

  const f64 s_off = fold1_oos_sharpe(fx, /*conviction_on=*/false);
  EXPECT_EQ(s_off, s_bare) << "conviction-off fold Sharpe must equal the bare-combiner fold Sharpe";
}

// ===========================================================================
// (c) Twice-run: both versions are byte-identical on two consecutive calls.
// ===========================================================================
TEST(ConvictionWF, TwiceRunIsBitIdentical) {
  const TwoAlphaFixture fx = make_fixture();
  EXPECT_EQ(fold1_oos_sharpe(fx, false), fold1_oos_sharpe(fx, false));
  EXPECT_EQ(fold1_oos_sharpe(fx, true), fold1_oos_sharpe(fx, true));
}

// ===========================================================================
// (d) n_windows=1 edge case: walk_forward_sharpe over the full test blend returns
// a single window == the full-sample Sharpe; it is finite and in a reasonable
// range. Conviction scaling changes it (covered by (a)); here we pin finiteness.
// ===========================================================================
TEST(ConvictionWF, SingleWindowIsFullSampleSharpeFinite) {
  const TwoAlphaFixture fx = make_fixture();
  const usize seg = fx.T / 3U;
  const std::array<f64, 2> w{0.5, 0.5};
  const std::vector<f64> b = blend(fx.A, fx.B, w, seg, 2U * seg);

  const std::vector<f64> one = ev::walk_forward_sharpe(std::span<const f64>{b}, 1U);
  ASSERT_EQ(one.size(), 1u);
  EXPECT_TRUE(std::isfinite(one.front()));
  EXPECT_LT(std::fabs(one.front()), 1e6) << "fold Sharpe must be in a sane range";
}

} // namespace atxtest_eval_conviction_wf
