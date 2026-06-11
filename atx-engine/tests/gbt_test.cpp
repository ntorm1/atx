// atx::engine::learn — deterministic histogram GBT learned alpha tests (S5-4).
//
// Suite Gbt — the load-bearing semantics of the deterministic, histogram
// gradient-boosted-tree learned alpha (Pattern-B edge 2):
//
//   1. DepthOneStump_MatchesBruteForceThreshold (M4) — a single-feature step
//      fixture (y jumps at x=0.5); fit_gbt_single_tree's split threshold matches
//      an INDEPENDENT O(n^2) brute-force best-threshold search within ~one bin
//      width. Non-vacuous: the brute force is its own reference, not the GBT's
//      output.
//   2. SameSeed_ByteIdenticalTreesAndSignal (M1, the make-or-break) — two fit_gbt
//      with the SAME seed produce an identical forest-node hash AND identical
//      predict_at signal at a date.
//   3. CapturesInteraction_BeatsLinearOnXorFixture — a pure-interaction fixture
//      (y = sign(f0)*sign(f1), NO marginal signal) where the GBT's OOS IC beats
//      the (no-interaction) linear model's: trees see what the linear model
//      cannot.
//   4. NoEdgePanel_RejectedByDeflation (M3) — a pure-noise fixture yields
//      oos_deflated_sharpe <= 0. GBT overfits hardest, so this proves the
//      deflation gate still rejects it.
//
// Fixtures are FeatureMatrix aggregates built directly (mirroring the S5-3
// linear_alpha_test make_panel pattern). Naming: Subject_Condition_Expected.

#include <cmath>  // std::fabs (stump threshold comparison)
#include <limits> // std::numeric_limits (tail NaN label)
#include <vector>

#include <Eigen/Dense> // Eigen::Index, MatX/VecX

#include <gtest/gtest.h>

#include "atx/core/hash.hpp"          // atx::core::hash_bytes
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, i32, u16, u32, u64, usize

#include "atx/engine/eval/cpcv.hpp"            // eval::CpcvConfig
#include "atx/engine/learn/elastic_net.hpp"    // ElasticNetCfg
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix
#include "atx/engine/learn/gbt.hpp"            // GbtCfg, fit_gbt, fit_gbt_single_tree, oos_ic
#include "atx/engine/learn/latent.hpp"         // LatentAugmentation
#include "atx/engine/learn/learned_source.hpp" // LearnedModel, GbtNode/Tree/Forest
#include "atx/engine/learn/linear_alpha.hpp"   // fit_linear, predict_at, oos_deflated_sharpe

namespace {

using atx::f64;
using atx::i32;
using atx::u16;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::learn::FeatureMatrix;
using atx::engine::learn::GbtCfg;
using atx::engine::learn::GbtForest;
using atx::engine::learn::GbtNode;
using atx::engine::learn::GbtTree;
using atx::engine::learn::LatentAugmentation;
using atx::engine::learn::LearnedModel;
namespace eval = atx::engine::eval;
namespace learn = atx::engine::learn;

// A small deterministic LCG so fixtures carry reproducible "noise" without an RNG
// dependency; pure function of its state (test-local). Mirrors the S5-3 fixture.
struct Lcg {
  u64 s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const u64 top = s >> 11U;
    const f64 u = static_cast<f64>(top) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0; // [-1, 1)
  }
};

// Build a FeatureMatrix with `n_dates` dates x `n_inst` instruments (all
// in-universe / valid). `label_fn(f0, f1, noise)` produces the horizon-h label
// from the planted features (feature 0, feature 1) and a per-row noise draw, with
// NaN at the unknowable tail (date + horizon >= n_dates). Remaining features (>=2)
// are independent noise columns.
template <typename LabelFn>
[[nodiscard]] FeatureMatrix make_fm(usize n_dates, usize n_inst, usize n_features,
                                    const std::vector<u16> &horizons, u64 seed, LabelFn label_fn) {
  FeatureMatrix fm;
  fm.n_dates = n_dates;
  fm.n_instruments = n_inst;
  fm.n_features = n_features;
  fm.Y.assign(horizons.size(), {});
  Lcg rng{seed};
  std::vector<f64> f0_of_row;
  std::vector<f64> f1_of_row;
  for (usize d = 0; d < n_dates; ++d) {
    for (usize i = 0; i < n_inst; ++i) {
      const usize row = fm.push_row(d, i);
      (void)row;
      const f64 f0 = rng.next();
      const f64 f1 = (n_features > 1U) ? rng.next() : 0.0;
      f0_of_row.push_back(f0);
      f1_of_row.push_back(f1);
      for (usize f = 0; f < n_features; ++f) {
        if (f == 0U) {
          fm.X.push_back(f0);
        } else if (f == 1U) {
          fm.X.push_back(f1);
        } else {
          fm.X.push_back(rng.next());
        }
      }
      fm.row_valid.push_back(1U);
    }
  }
  for (usize hi = 0; hi < horizons.size(); ++hi) {
    Lcg lrng{seed ^ (0x9ABCULL + hi)};
    for (usize r = 0; r < fm.n_rows(); ++r) {
      const usize d = fm.row_date[r];
      const usize ahead = d + static_cast<usize>(horizons[hi]);
      if (ahead >= n_dates) {
        fm.Y[hi].push_back(std::numeric_limits<f64>::quiet_NaN());
      } else {
        fm.Y[hi].push_back(label_fn(f0_of_row[r], f1_of_row[r], lrng.next()));
      }
    }
  }
  return fm;
}

// Hash every forest's node bytes (feature / threshold / leaf_value / structure)
// plus base + blend across all horizons into one digest. Non-vacuous: any change
// to a split feature, threshold, leaf value, child link, or leaf flag changes the
// buffer and so the digest.
[[nodiscard]] u64 hash_forests(const LearnedModel &m) {
  std::vector<f64> buf;
  for (const GbtForest &forest : m.forests) {
    buf.push_back(forest.base);
    for (const GbtTree &tree : forest.trees) {
      buf.push_back(static_cast<f64>(tree.nodes.size()));
      for (const GbtNode &node : tree.nodes) {
        buf.push_back(static_cast<f64>(node.feature));
        buf.push_back(node.threshold);
        buf.push_back(node.leaf_value);
        buf.push_back(static_cast<f64>(node.left));
        buf.push_back(static_cast<f64>(node.right));
        buf.push_back(node.is_leaf ? 1.0 : 0.0);
      }
    }
  }
  buf.insert(buf.end(), m.blend_w.begin(), m.blend_w.end());
  buf.push_back(static_cast<f64>(m.trial_count));
  // SAFETY: std::vector<f64> stores doubles contiguously; buf.data() points at
  // buf.size()*sizeof(f64) live bytes for the call's duration.
  return atx::core::hash_bytes(buf.data(), buf.size() * sizeof(f64));
}

[[nodiscard]] u64 hash_signal(const atx::core::linalg::VecX &v) {
  // SAFETY: Eigen VecX stores doubles contiguously; v.data() points at
  // size()*sizeof(f64) live bytes for the vector's lifetime.
  return atx::core::hash_bytes(v.data(), static_cast<usize>(v.size()) * sizeof(f64));
}

// A standard GBT cfg the M1/interaction/M3 tests share (single horizon).
[[nodiscard]] GbtCfg standard_cfg() {
  GbtCfg cfg;
  cfg.cpcv = eval::CpcvConfig{/*n_groups=*/5, /*n_test_groups=*/1, /*embargo=*/0.0};
  cfg.master_seed = 42ULL;
  cfg.horizons = {1};
  return cfg;
}

// ---- 1: M4 stump differential (brute-force reference) ------------------------

TEST(Gbt, DepthOneStump_MatchesBruteForceThreshold) {
  // One-feature step fixture: y jumps at x = 0.5. n points spread on [0, 1).
  const usize n = 200U;
  atx::core::linalg::MatX X(static_cast<Eigen::Index>(n), 1);
  atx::core::linalg::VecX y(static_cast<Eigen::Index>(n));
  for (usize i = 0; i < n; ++i) {
    const f64 x = static_cast<f64>(i) / static_cast<f64>(n); // [0, 1)
    X(static_cast<Eigen::Index>(i), 0) = x;
    y(static_cast<Eigen::Index>(i)) = (x >= 0.5) ? 1.0 : -1.0;
  }

  const u32 n_bins = 256U;
  const GbtTree tree = learn::fit_gbt_single_tree(X, y, /*max_depth=*/1U, n_bins);
  ASSERT_FALSE(tree.nodes.empty());
  const GbtNode &root = tree.nodes[0];
  ASSERT_FALSE(root.is_leaf) << "a depth-1 tree on a clean step must split";
  const f64 gbt_threshold = root.threshold;

  // Independent O(n^2) brute force: the threshold (a candidate midpoint between
  // adjacent sorted x) minimizing the total squared error of a two-leaf split.
  f64 best_thr = 0.0;
  f64 best_sse = std::numeric_limits<f64>::infinity();
  for (usize c = 0; c < n; ++c) {
    const f64 thr = X(static_cast<Eigen::Index>(c), 0);
    f64 sl = 0.0;
    f64 sr = 0.0;
    usize nl = 0U;
    usize nr = 0U;
    for (usize i = 0; i < n; ++i) {
      const f64 xv = X(static_cast<Eigen::Index>(i), 0);
      const f64 yv = y(static_cast<Eigen::Index>(i));
      if (xv < thr) {
        sl += yv;
        ++nl;
      } else {
        sr += yv;
        ++nr;
      }
    }
    if (nl == 0U || nr == 0U) {
      continue;
    }
    const f64 ml = sl / static_cast<f64>(nl);
    const f64 mr = sr / static_cast<f64>(nr);
    f64 sse = 0.0;
    for (usize i = 0; i < n; ++i) {
      const f64 xv = X(static_cast<Eigen::Index>(i), 0);
      const f64 yv = y(static_cast<Eigen::Index>(i));
      const f64 e = yv - ((xv < thr) ? ml : mr);
      sse += e * e;
    }
    if (sse < best_sse) {
      best_sse = sse;
      best_thr = thr;
    }
  }

  // Within ~two bin widths over the [0, 1) feature range (histogram quantization).
  const f64 bin_width = 1.0 / static_cast<f64>(n_bins);
  EXPECT_LE(std::fabs(gbt_threshold - best_thr), 2.0 * bin_width)
      << "gbt thr=" << gbt_threshold << " brute thr=" << best_thr;
}

// ---- 2: M1 determinism (make-or-break) ---------------------------------------

TEST(Gbt, SameSeed_ByteIdenticalTreesAndSignal) {
  // Planted-interaction fixture: y = sign(f0) * sign(f1).
  const auto xor_label = [](f64 f0, f64 f1, f64 /*noise*/) -> f64 {
    const f64 s0 = (f0 >= 0.0) ? 1.0 : -1.0;
    const f64 s1 = (f1 >= 0.0) ? 1.0 : -1.0;
    return s0 * s1;
  };
  const FeatureMatrix fm = make_fm(/*n_dates=*/30U, /*n_inst=*/10U, /*n_features=*/4U,
                                   /*horizons=*/{1}, /*seed=*/99ULL, xor_label);
  const LatentAugmentation aug;
  const GbtCfg cfg = standard_cfg();

  const LearnedModel m1 = learn::fit_gbt(fm, aug, cfg);
  const LearnedModel m2 = learn::fit_gbt(fm, aug, cfg);
  EXPECT_EQ(hash_forests(m1), hash_forests(m2)) << "same seed must produce byte-identical forests";

  const auto s1 = learn::predict_at(m1, fm, 10U);
  const auto s2 = learn::predict_at(m2, fm, 10U);
  ASSERT_EQ(s1.size(), s2.size());
  EXPECT_EQ(hash_signal(s1), hash_signal(s2)) << "same seed must produce identical signal";
}

// ---- 3: interaction capture (beats linear with no interaction aug) -----------

TEST(Gbt, CapturesInteraction_BeatsLinearOnXorFixture) {
  // Pure-interaction fixture: y = sign(f0)*sign(f1) with NO marginal signal — f0
  // and f1 are each uncorrelated with y, only their product carries it.
  const auto xor_label = [](f64 f0, f64 f1, f64 noise) -> f64 {
    const f64 s0 = (f0 >= 0.0) ? 1.0 : -1.0;
    const f64 s1 = (f1 >= 0.0) ? 1.0 : -1.0;
    return s0 * s1 + 0.10 * noise;
  };
  const FeatureMatrix fm = make_fm(/*n_dates=*/40U, /*n_inst=*/12U, /*n_features=*/4U,
                                   /*horizons=*/{1}, /*seed=*/7ULL, xor_label);
  const LatentAugmentation aug; // NO interaction augmentation -> the contrast is real

  const f64 gbt_ic = learn::oos_ic(learn::fit_gbt(fm, aug, standard_cfg()), fm);

  learn::LinearAlphaCfg lin_cfg;
  lin_cfg.en = learn::ElasticNetCfg{/*lambda=*/0.02, /*alpha=*/0.5, /*max_iter=*/2000, /*tol=*/1e-9};
  lin_cfg.use_ridge_baseline = false;
  lin_cfg.cpcv = eval::CpcvConfig{/*n_groups=*/5, /*n_test_groups=*/1, /*embargo=*/0.0};
  lin_cfg.master_seed = 42ULL;
  lin_cfg.horizons = {1};
  const f64 lin_ic = learn::oos_ic(learn::fit_linear(fm, aug, lin_cfg), fm);

  EXPECT_GT(gbt_ic, lin_ic) << "trees must capture the interaction the linear model cannot: gbt_ic="
                            << gbt_ic << " lin_ic=" << lin_ic;
}

// ---- 4: M3 deflation rejects pure noise --------------------------------------

TEST(Gbt, NoEdgePanel_RejectedByDeflation) {
  // Pure-noise fixture: the label is independent of every feature. Swept across
  // many seeds (each draws a wholly distinct noise panel — features AND labels) so
  // the deflation gate is proven SEED-ROBUST, not pinned to one lucky fixture. The
  // OOF dispersion floor (kOofDispersionFloor) is a tuned constant, so this is its
  // load-bearing guard: a no-edge GBT must score dsr <= 0 on EVERY panel.
  const auto noise_label = [](f64 /*f0*/, f64 /*f1*/, f64 noise) -> f64 { return noise; };
  const LatentAugmentation aug;
  for (u64 seed = 1; seed <= 24ULL; ++seed) {
    const FeatureMatrix fm = make_fm(/*n_dates=*/40U, /*n_inst=*/12U, /*n_features=*/4U,
                                     /*horizons=*/{1}, seed, noise_label);
    const LearnedModel m = learn::fit_gbt(fm, aug, standard_cfg());
    const f64 dsr = learn::oos_deflated_sharpe(m, fm);
    EXPECT_LE(dsr, 0.0) << "a no-edge GBT must not survive deflation: seed=" << seed
                        << " dsr=" << dsr;
  }
}

} // namespace
