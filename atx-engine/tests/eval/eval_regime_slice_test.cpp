// atx::engine::eval — regime_slice.hpp tests (S4.4a, suite EvalRegimeSlice).
//
// Proves the robustness measurement layer's SECOND half: a deterministic
// volatility-tercile regime partition + per-regime / walk-forward OOS Sharpe +
// the RobustnessVerdict that S4.5's gate will consume. The load-bearing fixture
// (RejectsHighVolCollapse) shows an alpha engineered to clear full-sample OOS
// but COLLAPSE in the high-vol regime is rejected by the verdict, while an alpha
// robust across all three terciles passes — the whole point of slicing OOS by
// regime instead of trusting one full-sample number.
//
// FIT/APPLY FIREWALL: every Sharpe here is computed on a DISJOINT slice of the
// SAME realized OOS PnL stream — no window/regime trains on its own test slice
// (the PnL is already the firewalled OOS output of extract_streams; the slicing
// is a pure partition, never a refit). Stated again in regime_slice.hpp.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/eval/regime_slice.hpp"

namespace {

using atx::f64;
using atx::u8;
using atx::usize;
using atx::engine::alpha::Panel;
using atx::engine::eval::kNumRegimes;
using atx::engine::eval::per_regime_sharpe;
using atx::engine::eval::regime_labels;
using atx::engine::eval::robustness_verdict;
using atx::engine::eval::RobustnessConfig;
using atx::engine::eval::RobustnessVerdict;
using atx::engine::eval::walk_forward_sharpe;

// A tiny deterministic LCG -> uniform(-1, 1), the S3/S4 fixture idiom (no RNG dep).
struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

// Build a single-instrument close panel from a per-date volatility schedule: each
// step multiplies price by (1 + sigma[t]*noise). A rising sigma schedule => the
// trailing realized vol rises => later dates land in the high-vol tercile.
[[nodiscard]] Panel vol_schedule_panel(const std::vector<f64> &sigma, std::uint64_t seed) {
  const usize dates = sigma.size();
  const usize insts = 1;
  std::vector<f64> close(dates * insts);
  f64 px = 100.0;
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    px *= (1.0 + sigma[t] * rng.next());
    close[t] = px;
  }
  auto r = Panel::create(dates, insts, {"close"}, {close}, {});
  EXPECT_TRUE(r.has_value()) << "vol-schedule panel must build";
  return std::move(r.value());
}

// =============================================================================
//  TercilePartitionIsBalancedAndOrdered — a panel whose trailing vol rises
//  monotonically over the back two-thirds gets the low/mid/high terciles in
//  ascending-date order, and each tercile holds ~1/3 of the LABELLED dates.
// =============================================================================
TEST(EvalRegimeSlice, TercilePartitionIsBalancedAndOrdered) {
  // A monotonically RISING vol schedule: low at the start, high at the end.
  std::vector<f64> sigma(90);
  for (usize t = 0; t < sigma.size(); ++t) {
    sigma[t] = 0.002 + 0.0006 * static_cast<f64>(t); // strictly increasing
  }
  const Panel panel = vol_schedule_panel(sigma, 0xC0FFEEu);

  RobustnessConfig cfg; // default vol_window
  const std::vector<u8> labels = regime_labels(panel, cfg.vol_window);
  ASSERT_EQ(labels.size(), panel.dates());

  // Count labelled (non-sentinel) dates per tercile. The warm-up dates (< vol
  // window) carry the kNoRegime sentinel and are excluded from the partition.
  std::array<usize, kNumRegimes> counts{};
  usize labelled = 0;
  for (const u8 g : labels) {
    if (g < kNumRegimes) {
      ++counts[g];
      ++labelled;
    }
  }
  ASSERT_GT(labelled, 0u);
  // Each tercile holds ~1/3 of the labelled dates (off-by-rounding tolerance).
  for (const usize c : counts) {
    EXPECT_NEAR(static_cast<f64>(c), static_cast<f64>(labelled) / 3.0,
                static_cast<f64>(labelled) / 6.0 + 1.0);
  }
  // Monotone-rising vol => the FIRST labelled date is low-vol, the LAST is high.
  usize first = labels.size();
  usize last = 0;
  for (usize t = 0; t < labels.size(); ++t) {
    if (labels[t] < kNumRegimes) {
      if (first == labels.size()) {
        first = t;
      }
      last = t;
    }
  }
  EXPECT_EQ(labels[first], 0u) << "earliest labelled date is the low-vol tercile";
  EXPECT_EQ(labels[last], kNumRegimes - 1) << "latest labelled date is the high-vol tercile";
}

// =============================================================================
//  Deterministic — same panel + window => byte-identical labels (no RNG / clock).
// =============================================================================
TEST(EvalRegimeSlice, LabelsAreDeterministic) {
  std::vector<f64> sigma(80, 0.01);
  for (usize t = 40; t < 80; ++t) {
    sigma[t] = 0.03;
  }
  const Panel panel = vol_schedule_panel(sigma, 0x1234u);
  const std::vector<u8> a = regime_labels(panel, 10);
  const std::vector<u8> b = regime_labels(panel, 10);
  EXPECT_EQ(a, b);
}

// =============================================================================
//  per_regime_sharpe partitions the SAME stream by label; a constant-mean stream
//  has equal per-regime Sharpe sign, and a label with no members yields 0.
// =============================================================================
TEST(EvalRegimeSlice, PerRegimeSharpeSlicesByLabel) {
  // PnL is +1 in regime 0/1 dates, -1 in regime 2 dates (with tiny jitter so std>0).
  const usize n = 30;
  std::vector<f64> pnl(n);
  std::vector<u8> labels(n);
  for (usize t = 0; t < n; ++t) {
    const u8 g = static_cast<u8>(t / 10); // 0,0..,1,1..,2,2..
    labels[t] = g;
    const f64 base = (g == 2) ? -1.0 : 1.0;
    pnl[t] = base + ((t % 2 == 0) ? 0.01 : -0.01);
  }
  const std::array<f64, kNumRegimes> sh =
      per_regime_sharpe<kNumRegimes>(std::span<const f64>{pnl}, std::span<const u8>{labels});
  EXPECT_GT(sh[0], 0.0);
  EXPECT_GT(sh[1], 0.0);
  EXPECT_LT(sh[2], 0.0) << "the high-vol regime PnL is negative => negative Sharpe";

  // A label set that never names regime 2 => regime-2 Sharpe is exactly 0 (empty).
  std::vector<u8> only01(n, 0u);
  for (usize t = 15; t < n; ++t) {
    only01[t] = 1u;
  }
  const std::array<f64, kNumRegimes> sh2 =
      per_regime_sharpe<kNumRegimes>(std::span<const f64>{pnl}, std::span<const u8>{only01});
  EXPECT_EQ(sh2[2], 0.0) << "an empty regime contributes a 0 Sharpe (degenerate)";
}

// =============================================================================
//  walk_forward_sharpe splits the stream into n contiguous windows; a stream
//  that flips sign at the midpoint shows opposite-signed first/last windows.
// =============================================================================
TEST(EvalRegimeSlice, WalkForwardWindowsSplitContiguously) {
  const usize n = 40;
  std::vector<f64> pnl(n);
  for (usize t = 0; t < n; ++t) {
    pnl[t] = (t < n / 2 ? 1.0 : -1.0) + ((t % 2 == 0) ? 0.02 : -0.02);
  }
  const std::vector<f64> w = walk_forward_sharpe(std::span<const f64>{pnl}, 4);
  ASSERT_EQ(w.size(), 4u);
  EXPECT_GT(w.front(), 0.0) << "first window is in the positive half";
  EXPECT_LT(w.back(), 0.0) << "last window is in the negative half";

  // Boundary: n_windows == 1 is the full-sample Sharpe; n_windows >= n caps to n.
  const std::vector<f64> one = walk_forward_sharpe(std::span<const f64>{pnl}, 1);
  EXPECT_EQ(one.size(), 1u);
  const std::vector<f64> capped = walk_forward_sharpe(std::span<const f64>{pnl}, 1000);
  EXPECT_LE(capped.size(), n) << "windows are capped at one period each";
  EXPECT_GT(capped.size(), 0u);
}

// =============================================================================
//  RejectsHighVolCollapse — THE load-bearing fixture. An alpha that is strongly
//  positive in low/mid vol but COLLAPSES (negative Sharpe) in the high-vol regime
//  can still post a POSITIVE full-sample Sharpe; robustness_verdict rejects it
//  because min-over-regimes falls below the floor. A second alpha positive in
//  EVERY regime passes. Same config, opposite verdicts.
// =============================================================================
TEST(EvalRegimeSlice, RejectsHighVolCollapse) {
  const usize n = 30;
  std::vector<u8> labels(n);
  for (usize t = 0; t < n; ++t) {
    labels[t] = static_cast<u8>(t / 10); // 10 dates per regime, ascending
  }

  // Collapsing alpha: +1.0 in regimes 0/1, strongly -1.5 in regime 2. The two
  // good regimes outweigh the bad one => full-sample Sharpe is POSITIVE, yet the
  // high-vol slice is a clear loser.
  std::vector<f64> collapse(n);
  for (usize t = 0; t < n; ++t) {
    const u8 g = labels[t];
    const f64 base = (g == 2) ? -1.5 : 1.0;
    collapse[t] = base + ((t % 2 == 0) ? 0.05 : -0.05);
  }
  // Robust alpha: +1.0 in EVERY regime (small jitter for non-zero variance).
  std::vector<f64> robust(n);
  for (usize t = 0; t < n; ++t) {
    robust[t] = 1.0 + ((t % 2 == 0) ? 0.05 : -0.05);
  }

  RobustnessConfig cfg;
  cfg.min_regime_sharpe = 0.0; // require non-negative per-period Sharpe in every slice
  cfg.n_walk_forward = 3;

  const RobustnessVerdict vc =
      robustness_verdict(std::span<const f64>{collapse}, std::span<const u8>{labels}, cfg);
  const RobustnessVerdict vr =
      robustness_verdict(std::span<const f64>{robust}, std::span<const u8>{labels}, cfg);

  // The collapsing alpha's FULL-SAMPLE Sharpe is positive (it would pass a naive
  // full-sample gate) ...
  EXPECT_GT(vc.full_sample_sharpe, 0.0) << "collapse alpha must clear a naive full-sample bar";
  // ... but it is NOT robust: its worst regime (high-vol) is below the floor.
  EXPECT_FALSE(vc.is_robust) << "high-vol collapse must be rejected by the regime slice";
  EXPECT_LT(vc.regime_sharpe[2], cfg.min_regime_sharpe);

  // The all-regime-positive alpha is robust.
  EXPECT_TRUE(vr.is_robust) << "an alpha positive in every regime + window passes";
  for (const f64 s : vr.regime_sharpe) {
    EXPECT_GE(s, cfg.min_regime_sharpe);
  }
}

// =============================================================================
//  WalkForwardCollapseAlsoRejects — an alpha robust across the three vol regimes
//  but that DIES in the last walk-forward window (a late regime break the tercile
//  partition does not see) is still rejected. Robustness = regimes AND windows.
// =============================================================================
TEST(EvalRegimeSlice, WalkForwardCollapseAlsoRejects) {
  const usize n = 36;
  // Labels balanced across the three regimes interleaved, so NO single regime is
  // all-late: the regime slice alone would pass.
  std::vector<u8> labels(n);
  for (usize t = 0; t < n; ++t) {
    labels[t] = static_cast<u8>(t % 3);
  }
  // PnL positive for the first 5/6 of the stream, sharply negative in the final
  // window => every regime's AVERAGE is fine, but the last walk-forward window is
  // a loser.
  std::vector<f64> pnl(n);
  for (usize t = 0; t < n; ++t) {
    pnl[t] = (t < (5 * n) / 6 ? 1.0 : -2.0) + ((t % 2 == 0) ? 0.05 : -0.05);
  }

  RobustnessConfig cfg;
  cfg.min_regime_sharpe = 0.0;
  cfg.n_walk_forward = 6;

  const RobustnessVerdict v =
      robustness_verdict(std::span<const f64>{pnl}, std::span<const u8>{labels}, cfg);
  // Regimes look fine (interleaved) ...
  for (const f64 s : v.regime_sharpe) {
    EXPECT_GE(s, cfg.min_regime_sharpe) << "interleaved labels keep every regime positive";
  }
  // ... but the late collapse sinks the final walk-forward window => not robust.
  EXPECT_LT(v.walk_forward_sharpe.back(), cfg.min_regime_sharpe);
  EXPECT_FALSE(v.is_robust)
      << "a late walk-forward collapse must reject even a regime-robust alpha";
}

} // namespace
