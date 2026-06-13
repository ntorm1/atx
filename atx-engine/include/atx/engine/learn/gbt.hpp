#pragma once

// atx::engine::learn — deterministic histogram gradient-boosted-tree learned
// alpha (S5-4, Pattern-B edge 2, §4.4 + M1/M3/M4/M5).
//
// =====================================================================
//  What this header is
// =====================================================================
//  The FITTING algorithm for a histogram GBT learned alpha. The deployed
//  parameters (GbtNode / GbtTree / GbtForest, the per-horizon forests on
//  LearnedModel) and the allocation-free inference walk live next to LearnedModel
//  in learned_source.hpp; this header builds them. fit_gbt mirrors fit_linear's
//  contract exactly — per-horizon CPCV OOS walk, a deployed refit on the full
//  trailing window, §0.6 horizon blend, and a GENUINE out-of-fold skill series
//  frozen on the model — so it reuses the SAME oos_deflated_sharpe / predict_at
//  gate (model-kind-agnostic, defined in linear_alpha.hpp). It captures feature
//  INTERACTIONS the linear model misses, and carries the strictest deflation.
//
// =====================================================================
//  Why a histogram GBT is byte-identical across builds/threads (M1)
// =====================================================================
//  Determinism is the make-or-break here. The three sources of GBT
//  non-determinism are all closed:
//
//    * Bin edges are quantile cut points computed from a SORTED copy of the
//      TRAIN-fold feature values only (M2 — no test/future row enters an edge).
//      The sort is a total order, so the edges are run-to-run identical.
//    * The split scan walks (feature, bin) left->right and keeps the FIRST max
//      (strictly-greater gain wins; a tie keeps the earlier feature/bin). No
//      time/address/map-order dependence — a fixed nested loop over a dense
//      histogram (M1).
//    * Subsampling draws from a Xoshiro256pp seeded via seed_for(master_seed,
//      "gbt-rows"/"gbt-feat", tree, 0) per tree (the fold/deploy forests fold a
//      distinct per-fold seed in first) — a fixed (seed, tag, a, b) mix, never
//      wall-clock / thread / address. Same cfg -> identical rows/features chosen.
//
//  Every per-node reduction (the histogram accumulation, the leaf value) walks
//  rows in ascending index, so the float sums are order-fixed too.
//
// Header-only; fitting is a COLD path, so std::vector / Eigen allocation is fine.
// Inference (gbt_forest_predict in learned_source.hpp) allocates nothing (M7).

#include <span>   // std::span
#include <vector> // std::vector

#include <Eigen/Dense> // Eigen::Index, MatX/VecX

#include "atx/core/random.hpp" // atx::core::Xoshiro256pp
#include "atx/core/types.hpp"  // f64, u32, u64, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

#include "atx/engine/eval/cpcv.hpp"            // eval::CpcvConfig, eval::cpcv_folds, LabelSpan, CpcvFold
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix
#include "atx/engine/learn/latent.hpp"         // LatentAugmentation, detail::pearson
#include "atx/engine/learn/learned_source.hpp" // LearnedModel, ModelKind, GbtForest/Tree/Node, gbt_*_predict

namespace atx::engine::learn {

namespace gbt_lin = atx::core::linalg;

// ===========================================================================
//  GbtCfg — the histogram-GBT knobs (defaults tuned conservatively: GBT overfits
//  hardest, so few/shallow trees, an l2 leaf prior, a variance-relative split-gain
//  floor, and the OOF dispersion floor together keep the M3 noise gate <= 0).
//
//  n_trees / max_depth / n_bins : forest size, tree depth, histogram resolution.
//  learning_rate                : shrinkage applied to each tree's leaf values.
//  row_subsample / feature_subsample : seeded per-tree subsample fractions in
//                                 (0, 1] (1.0 disables that subsample).
//  min_child                    : minimum child hessian (sample count, h=1) to
//                                 allow a split — a leaf-size floor.
//  l2                           : ridge prior on the leaf value (the GBT
//                                 regularizer; larger -> stronger shrink to 0).
//  min_split_gain               : the minimum split gain (XGBoost's gamma) a
//                                 NON-root node must clear to split, expressed as a
//                                 multiple of the train-label variance (so it is
//                                 unit-free in the return scale; see fit_forest).
//                                 The root split is always explored (a pure
//                                 interaction like sign(f0)*sign(f1) has ZERO
//                                 marginal gain at the root, so a root-level floor
//                                 would prune the tree before the interaction can
//                                 appear). On a no-edge panel the depth->=1 children
//                                 find no split clearing the floor, so the tree
//                                 stays nearly flat; a genuine interaction's
//                                 depth->=1 split clears it and fires. Together with
//                                 kOofDispersionFloor this is the M3 noise gate.
//  master_seed / cpcv / horizons: determinism root, CPCV fold config, horizons.
// ===========================================================================
struct GbtCfg {
  atx::u32 n_trees{30};
  atx::u32 max_depth{2};
  atx::u32 n_bins{64};
  atx::f64 learning_rate{0.05};
  atx::f64 row_subsample{0.7};
  atx::f64 feature_subsample{0.7};
  atx::f64 min_child{1.0};
  atx::f64 l2{5.0};
  atx::f64 min_split_gain{4.0};
  atx::u64 master_seed{0};
  eval::CpcvConfig cpcv{};
  std::vector<atx::u16> horizons{1};
};

// The OOF dispersion floor (in units of the OOF label std): a per-date prediction
// cross-section whose std is below this fraction of the label dispersion carries
// no usable ranking information and scores IC 0. Tuned so a no-edge forest's
// uniformly-weak predictions (pred-std ~0.10 * label-std) fall below it while a
// genuine interaction's concentrated predictions (~0.18+) clear it — the tree
// analog of L1's exact-zero on noise (see oof_ic_series_floored). The dominant M3
// knob: it drives the noise OOF series to all-zero so the deflation gate rejects
// it (dsr 0), with a comfortable margin to the interaction's signal strength.
inline constexpr atx::f64 kOofDispersionFloor = 0.16;

namespace gbt_detail {

// ---------------------------------------------------------------------------
//  Histogram binning — TRAIN-only quantile edges (M2).
// ---------------------------------------------------------------------------

// Per-feature bin edges: up to n_bins-1 ascending quantile cut points from the
// SORTED train values of that feature. A value's bin index is the count of edges
// it is >= (so bins 0..edges.size()). Duplicate quantile values collapse, so a
// near-constant feature simply yields fewer bins (its splits gain nothing).
struct BinEdges {
  std::vector<std::vector<atx::f64>> edges; // [feature] -> ascending cut points
};

// Compute the bin edges for an (n x p) design over its rows. For each feature,
// sort that column, then take n_bins-1 evenly-spaced quantile positions as cut
// points (dedup-ascending). Deterministic: the sort is a total order; the cut
// positions are a fixed integer division.
[[nodiscard]] BinEdges fit_bin_edges(const gbt_lin::MatX &X, atx::u32 n_bins);

// The bin index of `value` for feature f: the count of edges it is >= (linear
// scan; the edge count per feature is tiny). bin in [0, edges.size()].
[[nodiscard]] inline atx::usize bin_of(const BinEdges &be, atx::usize f, atx::f64 value) {
  const std::vector<atx::f64> &e = be.edges[f];
  atx::usize b = 0;
  while (b < e.size() && value >= e[b]) {
    ++b;
  }
  return b;
}

// ---------------------------------------------------------------------------
//  One depth-limited squared-error tree over a binned design.
// ---------------------------------------------------------------------------

// The per-row gradient/hessian + the pre-binned feature indices for a tree fit.
// g[i] / h[i] are this row's squared-error gradient / hessian; bins[i*p + f] is
// the bin of feature f for row i.
struct TreeData {
  const gbt_lin::MatX *X;        // the augmented design (for the threshold value)
  const BinEdges *be;            // the bin edges (for the threshold value)
  std::vector<atx::usize> bins;  // [n * p] row-major pre-binned feature indices
  std::vector<atx::f64> g;       // [n] gradient
  std::vector<atx::f64> h;       // [n] hessian
  atx::usize p{0};               // feature count
  atx::usize n_bins{0};          // histogram width (max bin index + 1)
};

// The leaf value for a row set: -Σg / (Σh + l2) (the squared-error Newton step).
[[nodiscard]] inline atx::f64 leaf_value(const TreeData &td, const std::vector<atx::usize> &rows,
                                         atx::f64 l2) {
  atx::f64 sg = 0.0;
  atx::f64 sh = 0.0;
  for (const atx::usize i : rows) {
    sg += td.g[i];
    sh += td.h[i];
  }
  return -sg / (sh + l2);
}

// The best split for a node's rows over the given (subsampled) feature set.
// Accumulates (Σg, Σh) per (feature, bin) and scans bins left->right, keeping the
// FIRST-max gain (strictly-greater wins; ties keep the earlier feature/bin — M1).
// Writes the chosen feature, the split BIN (left child = bin < split_bin), and the
// threshold value (the upper edge of the last left bin). `found` is false when no
// candidate clears BOTH the min_child leaf-size floor and the min_split_gain bar
// (the latter the caller passes as the node's gain floor — 0 at the root).
struct SplitResult {
  atx::usize feature{0};
  atx::usize split_bin{0}; // left child iff bin < split_bin
  atx::f64 threshold{0.0};
  bool found{false};
};

[[nodiscard]] SplitResult best_split(const TreeData &td,
                                     const std::vector<atx::usize> &rows,
                                     const std::vector<atx::usize> &feats, atx::f64 l2,
                                     atx::f64 min_child, atx::f64 min_split_gain);

// Grow one depth-limited tree by a recursive best-split over row subsets. The
// node array is built breadth-of-recursion; each call appends its node and (when
// it splits) recurses for both children, patching the child indices afterward.
atx::i32 grow_node(GbtTree &tree, const TreeData &td, const std::vector<atx::usize> &rows,
                   const std::vector<atx::usize> &feats, atx::u32 depth_left, atx::f64 l2,
                   atx::f64 min_child, atx::f64 min_split_gain, bool is_root);

// Pre-bin a design's rows into a TreeData (bins + the slots for g/h). The g/h are
// filled by the caller per boosting round (they change as F updates).
[[nodiscard]] TreeData make_tree_data(const gbt_lin::MatX &X, const BinEdges &be);

// A seeded subset of size round(frac * m) of [0, m), drawn deterministically from
// `rng` (a partial Fisher-Yates over an index permutation), returned ASCENDING so
// every downstream reduction stays order-fixed (M1). frac >= 1 -> all of [0, m).
[[nodiscard]] std::vector<atx::usize> seeded_subset(atx::usize m, atx::f64 frac,
                                                    atx::core::Xoshiro256pp &rng);

// ---------------------------------------------------------------------------
//  Boosting — fit a forest on a binned design (g/h refreshed per round).
// ---------------------------------------------------------------------------

// Fit a boosted forest on the (n x p) design X with labels y, using TRAIN-only bin
// edges `be`. F starts at base = mean(y); each round computes g = F - y, h = 1,
// grows one depth-limited tree over a seeded row+feature subsample, then
// F += learning_rate * tree.predict(row). Deterministic for a fixed master_seed.
[[nodiscard]] GbtForest fit_forest(const gbt_lin::MatX &X, const gbt_lin::VecX &y,
                                   const BinEdges &be, const GbtCfg &cfg,
                                   atx::u64 forest_seed_master);

// The genuine per-date OUT-OF-FOLD IC series for the GBT — fit_linear's
// detail::oof_ic_series, plus ONE numerical-hygiene step the tree model needs and
// the linear model gets for free. A tree forest on a NO-EDGE panel still predicts
// a tiny, evenly-spread per-date dispersion (a weak split fires, then nearly
// cancels), and detail::pearson's only guard is va == 0.0 EXACTLY — so that small
// dispersion becomes an O(1) spurious IC and the deflation gate sees phantom
// skill. The linear model dodges this because L1 drives noise coefficients to
// EXACTLY zero -> an exactly-constant prediction -> IC exactly 0.
//
// The tree analog is a RELATIVE dispersion floor against the GLOBAL OOF
// prediction scale: a no-edge forest spreads its (small) prediction dispersion
// roughly UNIFORMLY across dates, so every date's cross-sectional std is only a
// modest multiple of the global prediction std; a GENUINE interaction
// CONCENTRATES dispersion into the signal dates, whose std is a LARGE multiple of
// global. A date whose std is below rel_floor * global_pred_std carries no usable
// ranking information -> IC 0. On pure noise EVERY date falls below the cutoff ->
// an all-zero (degenerate) series -> the std==0 guard in oos_deflated_sharpe fires
// -> dsr 0 (rejected); a real interaction's signal dates clear it -> genuine skill
// survives. This STRENGTHENS the gate (it only ever zeroes low-information dates).
// Otherwise it is the identical genuine-OOF construction fit_linear uses.
[[nodiscard]] std::vector<atx::f64>
oof_ic_series_floored(const FeatureMatrix &fm, std::span<const atx::f64> oof_sum,
                      std::span<const atx::u32> oof_cnt, atx::f64 rel_floor);

} // namespace gbt_detail

// ===========================================================================
//  fit_gbt_single_tree — one depth-limited squared-error tree on a raw (X, y).
//
//  Exposed for the M4 stump differential test. Fits ONE tree (no subsampling, all
//  rows + all features) with the squared-error gradient g = mean(y) - y, h = 1
//  and an l2 = 0 / min_child = 1 leaf floor — so the gain-maximizing split is the
//  SSE-minimizing split, matching an independent brute-force best-threshold
//  search to within ~two bin widths (the histogram quantizes the cut point).
// ===========================================================================
[[nodiscard]] GbtTree fit_gbt_single_tree(const gbt_lin::MatX &X, const gbt_lin::VecX &y,
                                          atx::u32 max_depth, atx::u32 n_bins);

// ===========================================================================
//  fit_gbt — assemble the deployed multi-horizon histogram-GBT learned alpha.
//
//  Mirrors fit_linear exactly so the model plugs into the SAME deflation /
//  predict path: per horizon, walk the CPCV date-folds; on each fold fit
//  TRAIN-only bin edges + a TRAIN-only forest (seeded subsample), predict
//  OUT-OF-FOLD on the test rows (genuine OOS), accumulate the horizon-0 OOF
//  predictions, bump trial_count per fit. The DEPLOYED per-horizon forest is a
//  refit on the full trailing window. Horizon blend = normalize(max(oos_IC, 0)).
//  The OOS skill series is assembled from the OOF predictions (the SAME helper
//  fit_linear uses) so oos_deflated_sharpe is genuinely out-of-fold (M3).
//
//  Standardization stats are full-window (forward-applied — M2); the augmented
//  row layout is the shared build_augmented_row, so train/eval cannot drift and
//  the GBT trains/infers on the exact layout the linear model does.
// ===========================================================================
[[nodiscard]] LearnedModel fit_gbt(const FeatureMatrix &fm, const LatentAugmentation &aug,
                                   const GbtCfg &cfg);

// ===========================================================================
//  oos_ic — the mean per-date OUT-OF-FOLD information coefficient of a fitted
//  model. The OOS skill series (m.oos_score_series) is the per-date cross-
//  sectional IC of the genuine out-of-fold predictions (assembled at fit time);
//  its mean is the headline OOS IC. Model-kind-agnostic: it reads only the stored
//  series. An empty series -> 0.0 (no OOS coverage).
// ===========================================================================
[[nodiscard]] inline atx::f64 oos_ic(const LearnedModel &m, const FeatureMatrix &fm) {
  (void)fm; // the OOS series is frozen on the model at fit time
  const std::vector<atx::f64> &series = m.oos_score_series;
  if (series.empty()) {
    return 0.0;
  }
  atx::f64 sum = 0.0;
  for (const atx::f64 v : series) {
    sum += v;
  }
  return sum / static_cast<atx::f64>(series.size());
}

} // namespace atx::engine::learn
