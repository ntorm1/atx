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

#include <algorithm> // std::sort, std::fill
#include <cmath>     // std::isfinite, std::sqrt (OOF dispersion floor)
#include <cstddef>   // std::ptrdiff_t (subset slice index)
#include <span>      // std::span
#include <utility>   // std::move
#include <vector>    // std::vector

#include <Eigen/Dense> // Eigen::Index, MatX/VecX

#include "atx/core/macro.hpp"  // ATX_CHECK
#include "atx/core/random.hpp" // atx::core::Xoshiro256pp
#include "atx/core/types.hpp"  // f64, u32, u64, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

#include "atx/engine/eval/cpcv.hpp"            // eval::CpcvConfig, eval::cpcv_folds, LabelSpan, CpcvFold
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix
#include "atx/engine/learn/latent.hpp"         // LatentAugmentation, detail::pearson
#include "atx/engine/learn/learned_source.hpp" // LearnedModel, ModelKind, GbtForest/Tree/Node, gbt_*_predict
#include "atx/engine/learn/linear_alpha.hpp"   // detail::build_design, fit_standardization, pearson
#include "atx/engine/learn/train.hpp"          // seed_for, date_label_spans, expand_date_folds, RowFold

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
[[nodiscard]] inline BinEdges fit_bin_edges(const gbt_lin::MatX &X, atx::u32 n_bins) {
  const atx::usize n = static_cast<atx::usize>(X.rows());
  const atx::usize p = static_cast<atx::usize>(X.cols());
  BinEdges be;
  be.edges.assign(p, {});
  if (n == 0U || n_bins < 2U) {
    return be; // no rows, or a single bin -> no cut points (everything in bin 0)
  }
  const atx::usize n_cuts = static_cast<atx::usize>(n_bins) - 1U;
  std::vector<atx::f64> col(n);
  for (atx::usize f = 0; f < p; ++f) {
    for (atx::usize i = 0; i < n; ++i) {
      col[i] = X(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(f));
    }
    std::sort(col.begin(), col.end());
    std::vector<atx::f64> cuts;
    cuts.reserve(n_cuts);
    for (atx::usize c = 1; c <= n_cuts; ++c) {
      // The c-th of n_bins quantile boundaries -> the (c*n / n_bins)-th order stat.
      const atx::usize q = (c * n) / static_cast<atx::usize>(n_bins);
      const atx::usize idx = (q >= n) ? (n - 1U) : q;
      const atx::f64 edge = col[idx];
      if (cuts.empty() || edge > cuts.back()) {
        cuts.push_back(edge); // dedup-ascending: collapse repeated quantile values
      }
    }
    be.edges[f] = std::move(cuts);
  }
  return be;
}

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

[[nodiscard]] inline SplitResult best_split(const TreeData &td,
                                            const std::vector<atx::usize> &rows,
                                            const std::vector<atx::usize> &feats, atx::f64 l2,
                                            atx::f64 min_child, atx::f64 min_split_gain) {
  SplitResult out;
  // A split must clear min_split_gain to fire (XGBoost gamma): on noise the best
  // gain stays below the floor, so no split is taken and the node is a leaf.
  atx::f64 best_gain = min_split_gain;
  // Node totals (G, H) — the parent term of the gain.
  atx::f64 g_tot = 0.0;
  atx::f64 h_tot = 0.0;
  for (const atx::usize i : rows) {
    g_tot += td.g[i];
    h_tot += td.h[i];
  }
  const atx::f64 parent = (g_tot * g_tot) / (h_tot + l2);
  std::vector<atx::f64> gh(td.n_bins, 0.0); // per-bin Σg
  std::vector<atx::f64> hh(td.n_bins, 0.0); // per-bin Σh
  for (const atx::usize f : feats) {
    ATX_CHECK(f < td.p); // a subsampled feature indexes within the design (NDEBUG)
    std::fill(gh.begin(), gh.end(), 0.0);
    std::fill(hh.begin(), hh.end(), 0.0);
    for (const atx::usize i : rows) {
      const atx::usize b = td.bins[i * td.p + f];
      ATX_CHECK(b < td.n_bins); // bin index within the histogram width (NDEBUG)
      gh[b] += td.g[i];
      hh[b] += td.h[i];
    }
    // Scan split points left->right: left = bins [0, sb), right = [sb, n_bins).
    atx::f64 gl = 0.0;
    atx::f64 hl = 0.0;
    for (atx::usize sb = 1; sb < td.n_bins; ++sb) {
      gl += gh[sb - 1U];
      hl += hh[sb - 1U];
      const atx::f64 gr = g_tot - gl;
      const atx::f64 hr = h_tot - hl;
      if (hl < min_child || hr < min_child) {
        continue; // a child too small (leaf-size floor)
      }
      const atx::f64 gain =
          0.5 * ((gl * gl) / (hl + l2) + (gr * gr) / (hr + l2) - parent);
      if (gain > best_gain) { // strictly-greater wins -> FIRST-max tie-break (M1)
        best_gain = gain;
        out.feature = f;
        out.split_bin = sb;
        out.found = true;
      }
    }
  }
  if (out.found) {
    // Threshold = the upper edge of the last left bin (split_bin-1). A row goes
    // left iff its value < threshold (== its bin < split_bin). When split_bin-1 is
    // beyond the edge list (the top bin), there is no finite upper edge; that case
    // never produces a usable left/right partition under the bin scan above, so a
    // finite edge always exists here.
    const std::vector<atx::f64> &e = td.be->edges[out.feature];
    if (out.split_bin - 1U < e.size()) {
      out.threshold = e[out.split_bin - 1U];
    } else {
      out.threshold = e.empty() ? 0.0 : e.back();
    }
  }
  return out;
}

// Grow one depth-limited tree by a recursive best-split over row subsets. The
// node array is built breadth-of-recursion; each call appends its node and (when
// it splits) recurses for both children, patching the child indices afterward.
inline atx::i32 grow_node(GbtTree &tree, const TreeData &td, const std::vector<atx::usize> &rows,
                          const std::vector<atx::usize> &feats, atx::u32 depth_left, atx::f64 l2,
                          atx::f64 min_child, atx::f64 min_split_gain, bool is_root) {
  const atx::i32 self = static_cast<atx::i32>(tree.nodes.size());
  tree.nodes.push_back(GbtNode{}); // placeholder; patched below
  GbtNode node;
  node.is_leaf = true;
  node.leaf_value = leaf_value(td, rows, l2);
  if (depth_left == 0U || rows.size() < 2U) {
    tree.nodes[static_cast<atx::usize>(self)] = node;
    return self;
  }
  // The ROOT split is explored with no gain floor: a PURE interaction (e.g.
  // sign(f0)*sign(f1)) has ZERO marginal gain at the root, so a root-level floor
  // would prune the tree before the interaction can appear in a depth->=1 split.
  // Below the root the floor applies, so on a no-edge panel the depth->=1 children
  // find no qualifying split and stay leaves, while a genuine interaction's
  // depth->=1 split clears the floor and fires. `is_root` is true only for the
  // tree root (the boosting/stump entry points pass true; recursion passes false).
  const atx::f64 node_floor = is_root ? 0.0 : min_split_gain;
  const SplitResult sp = best_split(td, rows, feats, l2, min_child, node_floor);
  if (!sp.found) {
    tree.nodes[static_cast<atx::usize>(self)] = node;
    return self;
  }
  // Partition rows by the chosen split (ascending walk -> stable, deterministic).
  ATX_CHECK(sp.feature < td.p); // the split feature indexes within the design (NDEBUG)
  std::vector<atx::usize> left;
  std::vector<atx::usize> right;
  for (const atx::usize i : rows) {
    if (td.bins[i * td.p + sp.feature] < sp.split_bin) {
      left.push_back(i);
    } else {
      right.push_back(i);
    }
  }
  if (left.empty() || right.empty()) {
    tree.nodes[static_cast<atx::usize>(self)] = node; // degenerate -> leaf
    return self;
  }
  node.is_leaf = false;
  node.feature = static_cast<atx::u32>(sp.feature);
  node.threshold = sp.threshold;
  const atx::i32 lc = grow_node(tree, td, left, feats, depth_left - 1U, l2, min_child,
                                min_split_gain, /*is_root=*/false);
  const atx::i32 rc = grow_node(tree, td, right, feats, depth_left - 1U, l2, min_child,
                                min_split_gain, /*is_root=*/false);
  node.left = lc;
  node.right = rc;
  tree.nodes[static_cast<atx::usize>(self)] = node;
  return self;
}

// Pre-bin a design's rows into a TreeData (bins + the slots for g/h). The g/h are
// filled by the caller per boosting round (they change as F updates).
[[nodiscard]] inline TreeData make_tree_data(const gbt_lin::MatX &X, const BinEdges &be) {
  TreeData td;
  td.X = &X;
  td.be = &be;
  td.p = static_cast<atx::usize>(X.cols());
  const atx::usize n = static_cast<atx::usize>(X.rows());
  td.bins.assign(n * td.p, 0U);
  atx::usize max_bin = 0U;
  for (atx::usize i = 0; i < n; ++i) {
    for (atx::usize f = 0; f < td.p; ++f) {
      const atx::usize b =
          bin_of(be, f, X(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(f)));
      td.bins[i * td.p + f] = b;
      max_bin = (b > max_bin) ? b : max_bin;
    }
  }
  td.n_bins = max_bin + 1U;
  td.g.assign(n, 0.0);
  td.h.assign(n, 1.0); // squared-error hessian is 1
  return td;
}

// A seeded subset of size round(frac * m) of [0, m), drawn deterministically from
// `rng` (a partial Fisher-Yates over an index permutation), returned ASCENDING so
// every downstream reduction stays order-fixed (M1). frac >= 1 -> all of [0, m).
[[nodiscard]] inline std::vector<atx::usize> seeded_subset(atx::usize m, atx::f64 frac,
                                                           atx::core::Xoshiro256pp &rng) {
  std::vector<atx::usize> idx(m);
  for (atx::usize i = 0; i < m; ++i) {
    idx[i] = i;
  }
  atx::usize keep = (frac >= 1.0) ? m
                                  : static_cast<atx::usize>(frac * static_cast<atx::f64>(m) + 0.5);
  if (keep == 0U && m > 0U) {
    keep = 1U; // always keep at least one (a 0-row/0-feature node is useless)
  }
  if (keep >= m) {
    return idx; // already ascending [0, m)
  }
  // Partial Fisher-Yates: pick `keep` distinct indices into the first slots.
  for (atx::usize i = 0; i < keep; ++i) {
    const atx::usize span = m - i;
    const atx::usize j = i + static_cast<atx::usize>(rng.next_u64() % span);
    const atx::usize tmp = idx[i];
    idx[i] = idx[j];
    idx[j] = tmp;
  }
  std::vector<atx::usize> chosen(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(keep));
  std::sort(chosen.begin(), chosen.end()); // ascending -> order-fixed reductions
  return chosen;
}

// ---------------------------------------------------------------------------
//  Boosting — fit a forest on a binned design (g/h refreshed per round).
// ---------------------------------------------------------------------------

// Fit a boosted forest on the (n x p) design X with labels y, using TRAIN-only bin
// edges `be`. F starts at base = mean(y); each round computes g = F - y, h = 1,
// grows one depth-limited tree over a seeded row+feature subsample, then
// F += learning_rate * tree.predict(row). Deterministic for a fixed master_seed.
[[nodiscard]] inline GbtForest fit_forest(const gbt_lin::MatX &X, const gbt_lin::VecX &y,
                                          const BinEdges &be, const GbtCfg &cfg,
                                          atx::u64 forest_seed_master) {
  GbtForest forest;
  const atx::usize n = static_cast<atx::usize>(X.rows());
  if (n == 0U) {
    return forest;
  }
  TreeData td = make_tree_data(X, be);
  // base = mean(y); F initialized to base.
  atx::f64 base = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    base += y(static_cast<Eigen::Index>(i));
  }
  base /= static_cast<atx::f64>(n);
  forest.base = base;
  // The squared-error split gain scales with the label residual magnitude (gain ~
  // G^2/(H+l2), G a sum of gradients ~ label units), so the floor is scaled by the
  // train-label MEAN-square residual (Var(y) about base): a split must reduce the
  // node objective by at least min_split_gain * Var(y) to fire. Unit-free in the
  // return scale, and the dominant overfit guard — on a no-edge panel no split
  // clears this fraction-of-variance bar, so the forest stays at its constant base,
  // the OOF cross-section is flat, every per-date IC is 0 (degenerate series), and
  // the deflation gate cleanly rejects it (the tree analog of L1's exact-zeroing).
  // A genuine interaction explains a large fraction of variance and fires.
  atx::f64 var_y = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    const atx::f64 dv = y(static_cast<Eigen::Index>(i)) - base;
    var_y += dv * dv;
  }
  var_y /= static_cast<atx::f64>(n); // population variance of the label about base
  const atx::f64 gain_floor = cfg.min_split_gain * var_y;
  std::vector<atx::f64> F(n, base);
  for (atx::u32 t = 0; t < cfg.n_trees; ++t) {
    // Squared-error gradient/hessian at the current F.
    for (atx::usize i = 0; i < n; ++i) {
      td.g[i] = F[i] - y(static_cast<Eigen::Index>(i));
      td.h[i] = 1.0;
    }
    // Seeded row + feature subsample (distinct streams via the tag's a/b).
    atx::core::Xoshiro256pp rrng{seed_for(forest_seed_master, "gbt-rows", t, 0U)};
    atx::core::Xoshiro256pp frng{seed_for(forest_seed_master, "gbt-feat", t, 0U)};
    const std::vector<atx::usize> rows = seeded_subset(n, cfg.row_subsample, rrng);
    const std::vector<atx::usize> feats = seeded_subset(td.p, cfg.feature_subsample, frng);
    GbtTree tree;
    grow_node(tree, td, rows, feats, cfg.max_depth, cfg.l2, cfg.min_child, gain_floor,
              /*is_root=*/true);
    // Shrink the tree's leaf values by the learning rate, then update F over ALL
    // rows (subsampling affects which rows the tree was FIT on, not who it scores).
    for (GbtNode &node : tree.nodes) {
      if (node.is_leaf) {
        node.leaf_value *= cfg.learning_rate;
      }
    }
    for (atx::usize i = 0; i < n; ++i) {
      // SAFETY: Eigen X is column-major, so row i is NOT contiguous — copy its p
      // finite cells into a fresh p-element buffer and span THAT (valid for the
      // gbt_tree_predict call); no aliasing of X.
      std::vector<atx::f64> row(td.p);
      for (atx::usize f = 0; f < td.p; ++f) {
        row[f] = X(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(f));
      }
      F[i] += gbt_tree_predict(tree, std::span<const atx::f64>{row});
    }
    forest.trees.push_back(std::move(tree));
  }
  return forest;
}

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
[[nodiscard]] inline std::vector<atx::f64>
oof_ic_series_floored(const FeatureMatrix &fm, std::span<const atx::f64> oof_sum,
                      std::span<const atx::u32> oof_cnt, atx::f64 rel_floor) {
  const atx::usize nr = fm.n_rows();
  // The prediction-energy scale the per-date floor is relative to: the std of the
  // horizon-0 LABEL over the covered OOF rows. A no-edge forest's OOF predictions
  // carry tiny energy RELATIVE to the label dispersion (weak, learning-rate-shrunk
  // splits), while a genuine interaction's predictions carry a sizeable fraction of
  // it. Scaling by the label std (not the prediction's own std) makes the floor a
  // SIGNAL-STRENGTH test, not a self-referential dispersion ratio (which cannot
  // distinguish noise from signal — both spread evenly across dates). Order-fixed
  // ascending reduction (M1).
  atx::f64 lab_mean = 0.0;
  atx::usize lab_cnt = 0;
  for (atx::usize i = 0; i < nr; ++i) {
    if (oof_cnt[i] != 0U && std::isfinite(fm.Y[0][i])) {
      lab_mean += fm.Y[0][i];
      ++lab_cnt;
    }
  }
  atx::f64 label_std = 0.0;
  if (lab_cnt > 0U) {
    lab_mean /= static_cast<atx::f64>(lab_cnt);
    atx::f64 lv = 0.0;
    for (atx::usize i = 0; i < nr; ++i) {
      if (oof_cnt[i] != 0U && std::isfinite(fm.Y[0][i])) {
        const atx::f64 d = fm.Y[0][i] - lab_mean;
        lv += d * d;
      }
    }
    label_std = std::sqrt(lv / static_cast<atx::f64>(lab_cnt));
  }
  std::vector<atx::f64> series;
  atx::usize r = 0;
  while (r < nr) {
    const atx::usize date = fm.row_date[r];
    std::vector<atx::f64> pv;
    std::vector<atx::f64> lv;
    atx::usize e = r;
    for (; e < nr && fm.row_date[e] == date; ++e) {
      if (oof_cnt[e] == 0U) {
        continue;
      }
      const atx::f64 label = fm.Y[0][e];
      if (!std::isfinite(label)) {
        continue;
      }
      pv.push_back(oof_sum[e] / static_cast<atx::f64>(oof_cnt[e]));
      lv.push_back(label);
    }
    if (pv.size() >= 2U) {
      // Relative dispersion of the prediction cross-section vs the GLOBAL OOF
      // prediction scale. A date whose cross-sectional std is below rel_floor times
      // the global prediction std carries no usable ranking information -> IC 0.
      // This is the tree analog of L1's exact-zeroing: a no-edge forest's per-date
      // dispersion is a small fraction of its global dispersion (uniform noise),
      // while a genuine interaction concentrates dispersion into the signal dates
      // (a large multiple of global). The cutoff cleanly separates the two.
      atx::f64 mn = 0.0;
      for (const atx::f64 v : pv) {
        mn += v;
      }
      mn /= static_cast<atx::f64>(pv.size());
      atx::f64 var = 0.0;
      for (const atx::f64 v : pv) {
        var += (v - mn) * (v - mn);
      }
      const atx::f64 std_dev = std::sqrt(var / static_cast<atx::f64>(pv.size()));
      if (std_dev <= rel_floor * label_std) {
        series.push_back(0.0); // sub-signal-strength dispersion -> no skill
      } else {
        series.push_back(
            detail::pearson(std::span<const atx::f64>{pv}, std::span<const atx::f64>{lv}));
      }
    }
    r = e;
  }
  return series;
}

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
[[nodiscard]] inline GbtTree fit_gbt_single_tree(const gbt_lin::MatX &X, const gbt_lin::VecX &y,
                                                 atx::u32 max_depth, atx::u32 n_bins) {
  GbtTree tree;
  const atx::usize n = static_cast<atx::usize>(X.rows());
  if (n == 0U) {
    return tree;
  }
  const gbt_detail::BinEdges be = gbt_detail::fit_bin_edges(X, n_bins);
  gbt_detail::TreeData td = gbt_detail::make_tree_data(X, be);
  atx::f64 mean_y = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    mean_y += y(static_cast<Eigen::Index>(i));
  }
  mean_y /= static_cast<atx::f64>(n);
  for (atx::usize i = 0; i < n; ++i) {
    td.g[i] = mean_y - y(static_cast<Eigen::Index>(i)); // residual gradient about the mean
    td.h[i] = 1.0;
  }
  std::vector<atx::usize> rows(n);
  std::vector<atx::usize> feats(td.p);
  for (atx::usize i = 0; i < n; ++i) {
    rows[i] = i;
  }
  for (atx::usize f = 0; f < td.p; ++f) {
    feats[f] = f;
  }
  gbt_detail::grow_node(tree, td, rows, feats, max_depth, /*l2=*/0.0, /*min_child=*/1.0,
                        /*min_split_gain=*/0.0, /*is_root=*/true);
  return tree;
}

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
[[nodiscard]] inline LearnedModel fit_gbt(const FeatureMatrix &fm, const LatentAugmentation &aug,
                                          const GbtCfg &cfg) {
  LearnedModel m;
  m.kind = ModelKind::Gbt;
  m.aug = aug;
  m.n_base_features = static_cast<atx::u32>(fm.n_features);
  m.horizons = cfg.horizons;
  m.trial_count = 0;

  // Deployed full-window standardization (the forward-applied transform, M2).
  std::vector<atx::usize> all_valid;
  for (atx::usize r = 0; r < fm.n_rows(); ++r) {
    if (fm.row_valid[r] != 0) {
      all_valid.push_back(r);
    }
  }
  detail::fit_standardization(fm, std::span<const atx::usize>{all_valid}, m.feat_mean, m.feat_sd);

  m.forests.assign(cfg.horizons.size(), GbtForest{});
  m.blend_w.assign(cfg.horizons.size(), 0.0);
  std::vector<atx::f64> oos_ic_h(cfg.horizons.size(), 0.0);

  // Horizon-0 OOF accumulators keyed by FeatureMatrix row (average across folds).
  std::vector<atx::f64> oof_pred_sum(fm.n_rows(), 0.0);
  std::vector<atx::u32> oof_pred_cnt(fm.n_rows(), 0U);

  for (atx::usize h = 0; h < cfg.horizons.size(); ++h) {
    const std::vector<eval::LabelSpan> spans = date_label_spans(fm, cfg.horizons[h]);
    const std::vector<eval::CpcvFold> dfolds =
        eval::cpcv_folds(std::span<const eval::LabelSpan>{spans}, cfg.cpcv);
    const Folds folds = expand_date_folds(dfolds, fm);

    std::vector<atx::f64> oos_pred;
    std::vector<atx::f64> oos_label;
    atx::usize fold_idx = 0;
    for (const RowFold &f : folds) {
      // Fold-local standardization on the TRAIN rows only (M2), applied forward to
      // both the train and OOS test design.
      LearnedModel fold_shell = m;
      detail::fit_standardization(fm, std::span<const atx::usize>{f.train_rows},
                                  fold_shell.feat_mean, fold_shell.feat_sd);
      gbt_lin::MatX Xtr;
      gbt_lin::VecX ytr;
      const atx::usize ntr = detail::build_design(
          fm, fold_shell, std::span<const atx::usize>{f.train_rows}, h, Xtr, ytr);
      if (ntr == 0U) {
        ++fold_idx;
        continue;
      }
      // TRAIN-only bin edges, then a fold-local boosted forest (seeded per fold so
      // distinct folds draw distinct subsamples but are reproducible — M1).
      const gbt_detail::BinEdges be = gbt_detail::fit_bin_edges(Xtr, cfg.n_bins);
      const atx::u64 fold_seed = seed_for(cfg.master_seed, "gbt-fold", h, fold_idx);
      const GbtForest forest = gbt_detail::fit_forest(Xtr, ytr, be, cfg, fold_seed);
      ++m.trial_count; // one distinct fit -> one deflation trial (§0.3)

      gbt_lin::MatX Xte;
      gbt_lin::VecX yte;
      std::vector<atx::usize> te_rows;
      const atx::usize nte = detail::build_design(
          fm, fold_shell, std::span<const atx::usize>{f.test_rows}, h, Xte, yte, &te_rows);
      std::vector<atx::f64> row(static_cast<atx::usize>(Xte.cols()));
      for (atx::usize i = 0; i < nte; ++i) {
        for (atx::usize j = 0; j < row.size(); ++j) {
          row[j] = Xte(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j));
        }
        const atx::f64 pred = gbt_forest_predict(forest, std::span<const atx::f64>{row});
        oos_pred.push_back(pred);
        oos_label.push_back(yte(static_cast<Eigen::Index>(i)));
        if (h == 0U) {
          oof_pred_sum[te_rows[i]] += pred;
          oof_pred_cnt[te_rows[i]] += 1U;
        }
      }
      ++fold_idx;
    }
    oos_ic_h[h] = detail::pearson(std::span<const atx::f64>{oos_pred},
                                  std::span<const atx::f64>{oos_label});

    // Deployed per-horizon forest: refit on the full trailing window with m's
    // full-window standardization + full-window bin edges (forward-applied, M2).
    gbt_lin::MatX Xfull;
    gbt_lin::VecX yfull;
    const atx::usize nfull =
        detail::build_design(fm, m, std::span<const atx::usize>{all_valid}, h, Xfull, yfull);
    if (nfull > 0U) {
      const gbt_detail::BinEdges be = gbt_detail::fit_bin_edges(Xfull, cfg.n_bins);
      const atx::u64 deploy_seed = seed_for(cfg.master_seed, "gbt-deploy", h, 0U);
      m.forests[h] = gbt_detail::fit_forest(Xfull, yfull, be, cfg, deploy_seed);
    }
  }

  // Genuine per-date OOS skill series from the horizon-0 OOF predictions — the same
  // genuine-OOF construction fit_linear uses, with the tree-model dispersion floor
  // (see gbt_detail::oof_ic_series_floored) so a no-edge cross-section whose
  // prediction energy is below signal strength scores IC 0. Frozen for the gate (M3).
  m.oos_score_series = gbt_detail::oof_ic_series_floored(
      fm, std::span<const atx::f64>{oof_pred_sum}, std::span<const atx::u32>{oof_pred_cnt},
      /*rel_floor=*/kOofDispersionFloor);

  // §0.6 horizon blend: normalize(max(oos_IC_h, 0)). All non-positive -> uniform.
  atx::f64 sum = 0.0;
  for (atx::usize h = 0; h < cfg.horizons.size(); ++h) {
    const atx::f64 w = (oos_ic_h[h] > 0.0) ? oos_ic_h[h] : 0.0;
    m.blend_w[h] = w;
    sum += w;
  }
  if (sum > 0.0) {
    for (atx::f64 &w : m.blend_w) {
      w /= sum;
    }
  } else {
    const atx::f64 u =
        (cfg.horizons.empty()) ? 0.0 : 1.0 / static_cast<atx::f64>(cfg.horizons.size());
    for (atx::f64 &w : m.blend_w) {
      w = u;
    }
  }
  return m;
}

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
